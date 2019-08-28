/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2008, 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


#include "mariadb.h"
#include "sql_select.h"
#include "opt_trace.h"

int test_if_order_by_key(JOIN *join, ORDER *order, TABLE *table, uint idx,
                         uint *used_key_parts= NULL);
COND* substitute_for_best_equal_field(THD *thd, JOIN_TAB *context_tab,
                                      COND *cond,
                                      COND_EQUAL *cond_equal,
                                      void *table_join_idx,
                                      bool do_substitution);
enum_nested_loop_state
end_nest_materialization(JOIN *join, JOIN_TAB *join_tab, bool end_of_records);
bool get_range_limit_read_cost(const JOIN_TAB *tab, const TABLE *table,
                               ha_rows table_records, uint keynr,
                               ha_rows rows_limit, double *read_time);
Item **get_sargable_cond(JOIN *join, TABLE *table);

/*
  Substitute base table items with nest's table items

  SYNOPSIS

  substitute_base_with_nest_items()
    @param join          the join handler

  DESCRIPTION
    Substitute base table items for the tables inside the nest
    with the nest items. This is needed when an expression needs
    to be evaluated in the post ORDER BY context

    Example
    select * from t1,t2,t3
      where t1.a=t2.a and t2.b=t3.b
      order by t1.a,t2.a limit 5;

    let's say in this case the join order is t1,t2,t3 and this uses a
    sort-nest. So the actual representation would be sort_nest(t1,t2),t3.

    Now look at this equality condition t2.b = t3.b, this would
    be evaluated in the post ORDER BY context. So t2.b should
    actually refer to the sort-nest items instead of base table items.
    This is why we need to substitute base table items with sort-nest items.

    For the equality condition t1.a = t2.a there is no need for substitution
    as this condition is internal to the nest, that is this condition will be
    evaluated in the pre ORDER BY context.

    This function does the substitution for
      - WHERE clause
      - SELECT LIST
      - ORDER BY clause
      - ON expression
      - REF access items

*/
void substitute_base_with_nest_items(JOIN *join)
{
  THD *thd= join->thd;
  SORT_NEST_INFO *sort_nest_info= join->sort_nest_info;
  REPLACE_NEST_FIELD_ARG arg= {join};

  List_iterator<Item> it(join->fields_list);
  Item *item, *new_item;
  while ((item= it++))
  {
    if ((new_item= item->transform(thd,
                                   &Item::replace_with_nest_items, TRUE,
                                   (uchar *) &arg)) != item)
    {
      new_item->name= item->name;
      thd->change_item_tree(it.ref(), new_item);
    }
    new_item->update_used_tables();
  }

  ORDER *ord;
  for (ord= join->order; ord ; ord=ord->next)
  {
    (*ord->item)= (*ord->item)->transform(thd,
                                          &Item::replace_with_nest_items,
                                          TRUE, (uchar *) &arg);
    (*ord->item)->update_used_tables();
  }

  JOIN_TAB *end_tab= sort_nest_info->nest_tab;
  uint i, j;
  for (i= join->const_tables + sort_nest_info->n_tables, j=0;
       i < join->top_join_tab_count; i++, j++)
  {
    JOIN_TAB *tab= end_tab + j;
    if (tab->type == JT_REF || tab->type == JT_EQ_REF ||
        tab->type == JT_REF_OR_NULL)
    {
      for (uint keypart= 0; keypart < tab->ref.key_parts; keypart++)
      {
        item= tab->ref.items[keypart]->transform(thd,
                                                 &Item::replace_with_nest_items,
                                                 TRUE, (uchar *) &arg);
        if (item != tab->ref.items[keypart])
        {
          tab->ref.items[keypart]= item;
          Item *real_item= item->real_item();
          store_key *key_copy= tab->ref.key_copy[keypart];
          if (key_copy->type() == store_key::FIELD_STORE_KEY)
          {
            store_key_field *field_copy= ((store_key_field *)key_copy);
            DBUG_ASSERT(real_item->type() == Item::FIELD_ITEM);
            field_copy->change_source_field((Item_field *) real_item);
          }
        }
      }
    }

    if (*tab->on_expr_ref)
    {
      item= (*tab->on_expr_ref)->transform(thd,
                                           &Item::replace_with_nest_items,
                                           TRUE, (uchar *) &arg);
      *tab->on_expr_ref= item;
      (*tab->on_expr_ref)->update_used_tables();
    }
    if (tab->bush_children)
      substitutions_for_sjm_lookup(join, tab);
  }

  extract_condition_for_the_nest(join);
  Item *conds= join->conds;
  if (conds)
  {
    conds= conds->transform(thd, &Item::replace_with_nest_items, TRUE,
                            (uchar *) &arg);
    conds->update_used_tables();
  }
  join->conds= conds;
}


void substitutions_for_sjm_lookup(JOIN *join, JOIN_TAB *sjm_tab)
{
  JOIN_TAB *tab= sjm_tab->bush_children->start;
  TABLE_LIST *emb_sj_nest= tab->table->pos_in_table_list->embedding;

  /*
    Walk out of outer join nests until we reach the semi-join nest
    we're in
  */
  while (!emb_sj_nest->sj_mat_info)
    emb_sj_nest= emb_sj_nest->embedding;
  SJ_MATERIALIZATION_INFO *sjm= emb_sj_nest->sj_mat_info;
  THD *thd= join->thd;

  if (!sjm->is_sj_scan)
  {
    Item *left_expr= emb_sj_nest->sj_subq_pred->left_expr;
    REPLACE_NEST_FIELD_ARG arg= {join};
    left_expr= left_expr->transform(thd, &Item::replace_with_nest_items,
                                    TRUE, (uchar *)&arg);
    left_expr->update_used_tables();
    emb_sj_nest->sj_subq_pred->left_expr= left_expr;
  }
}


/*
  Extract from the WHERE clause the part which is internal to the sort-nest

  SYNOPSIS
  extract_condition_for_the_nest()
    @param join          the join handler

  DESCRIPTION
    Extract the condition from the WHERE clause that can be evaluated by the
    tables inside the sort-nest.

  Example
    select * from t1,t2,t3
      where t1.a=t2.a and t2.b=t3.b
      order by t1.a,t2.a limit 5;

    let's say in this case the join order is t1,t2,t3 and this uses a
    sort-nest. So the actual representation would be sort_nest(t1,t2),t3.

    The WHERE clause is t1.a=t2.a and t2.b=t3.b, so here we like to extract
    the condition t1.a=t2.a from the WHERE clause because it can be evaluated
    in the pre ORDER BY contest by the tables that would be in the sort-nest.

    The extracted condition is stored inside the structure SORT_NEST_INFO.
    Also we remove the extracted condition from the WHERE clause.

    So after this call the
    Extracted condition would be t1.a=t2.a and the
    WHERE clause would be t2.b=t3.b

*/

void extract_condition_for_the_nest(JOIN *join)
{
  SORT_NEST_INFO *sort_nest_info= join->sort_nest_info;
  Item *orig_cond= join->conds;
  if (!sort_nest_info)
    return;
  THD *thd= join->thd;
  Item *extracted_cond;
  SELECT_LEX* sl= join->select_lex;

  /*
    check_cond_extraction_for_nest would set NO_EXTRACTION_FL for
    all the items that cannot be added to the inner tables of the nest
  */
  check_cond_extraction_for_nest(thd, orig_cond,
                                 &Item::pushable_cond_checker_for_nest,
                                 (uchar *)(&sort_nest_info->nest_tables_map));
  /*
    build_cond_for_grouping_fields would create the entire
    condition that would be added to the tables inside the nest.
    This may clone some items too.
  */
  extracted_cond= sl->build_cond_for_grouping_fields(thd, orig_cond, TRUE);

  if (extracted_cond)
  {
    if (extracted_cond->fix_fields_if_needed(thd, 0))
      return;
    extracted_cond->update_used_tables();
    /*
      Remove from the WHERE clause all the conditions that were added
      to the inner tables of the sort nest
    */
    orig_cond= remove_pushed_top_conjuncts(thd, orig_cond);
    sort_nest_info->nest_cond= extracted_cond;
  }
  join->conds= orig_cond;
}


/**
  @brief
   For a condition check possibility of exraction a formula over sort-nest
   base table items

  @param thd      The thread handle
  @param cond     The condition whose subformulas are to be analyzed
  @param checker  The checker callback function to be applied to the nodes
                  of the tree of the object

  @details
    This method traverses the AND-OR condition cond and for each subformula of
    the condition it checks whether it can be usable for the extraction of a
    condition over the sort_nest base tables.
    The method uses the call-back parameter checker to check whether a
    primary formula depends only on the sort-nest tables or not.
    The subformulas that are not usable are marked with the
    flag NO_EXTRACTION_FL.
    The subformulas that can be entierly extracted are marked with the flag
    FULL_EXTRACTION_FL.

  @note
    The flag NO_EXTRACTION_FL set in a subformula allows to avoid
    building clone for the subformula which cannot be extracted.
    The flag FULL_EXTRACTION_FL allows to delete later all top level conjuncts
    from cond.
*/

void
check_cond_extraction_for_nest(THD *thd, Item *cond,
                               Pushdown_checker checker, uchar* arg)
{
  if (cond->get_extraction_flag() == NO_EXTRACTION_FL)
    return;
  cond->clear_extraction_flag();
  if (cond->type() == Item::COND_ITEM)
  {
    Item_cond_and *and_cond=
      (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC) ?
      ((Item_cond_and*) cond) : 0;

    List<Item> *arg_list=  ((Item_cond*) cond)->argument_list();
    List_iterator<Item> li(*arg_list);
    uint count= 0;         // to count items not containing NO_EXTRACTION_FL
    uint count_full= 0;    // to count items with FULL_EXTRACTION_FL
    Item *item;
    while ((item=li++))
    {
      check_cond_extraction_for_nest(thd, item, checker, arg);
      if (item->get_extraction_flag() !=  NO_EXTRACTION_FL)
      {
        count++;
        if (item->get_extraction_flag() == FULL_EXTRACTION_FL)
          count_full++;
      }
      else if (!and_cond)
        break;
    }
    if ((and_cond && count == 0) || item)
      cond->set_extraction_flag(NO_EXTRACTION_FL);
    if (count_full == arg_list->elements)
    {
      cond->set_extraction_flag(FULL_EXTRACTION_FL);
    }
    if (cond->get_extraction_flag() != 0)
    {
      li.rewind();
      while ((item=li++))
        item->clear_extraction_flag();
    }
  }
  else
  {
    int fl= (cond->*checker)(arg) ?
      FULL_EXTRACTION_FL : NO_EXTRACTION_FL;
    cond->set_extraction_flag(fl);
  }
}


/*
  Propgate all the multiple equalites for the order by items,
  so that one can use them to generate QEP that would
  also take into consideration equality propagation.

  Example
    select * from t1,t2 where t1.a=t2.a order by t1.a

  So the possible join orders would be:

  t1 join t2 then sort
  t2 join t1 then sort
  t1 sort(t1) join t2
  t2 sort(t2) join t1 => this is only possible when equality propagation is
                         performed

  @param join           JOIN handler
  @param sort_order     the ORDER BY clause
*/

void propagate_equal_field_for_orderby(JOIN *join, ORDER *first_order)
{
  if (!join->sort_nest_allowed())
    return;
  ORDER *order;
  for (order= first_order; order; order= order->next)
  {
    if (optimizer_flag(join->thd, OPTIMIZER_SWITCH_ORDERBY_EQ_PROP) &&
        join->cond_equal)
    {
      Item *item= order->item[0];
      /*
        TODO: equality substitution in the context of ORDER BY is
        sometimes allowed when it is not allowed in the general case.
        We make the below call for its side effect: it will locate the
        multiple equality the item belongs to and set item->item_equal
        accordingly.
      */
      (void)item->propagate_equal_fields(join->thd,
                                         Value_source::
                                         Context_identity(),
                                         join->cond_equal);
    }
  }
}


/*
  Checks if by considering the current join_tab
  would the prefix of the join order satisfy
  the ORDER BY clause.

  @param join             JOIN handler
  @param join_tab         joined table to check if addition of this
                          table in the join order would achieve
                          the ordering
  @param previous_tables  table_map for all the tables in the prefix
                          of the current partial plan

  @retval
   TRUE   ordering is achieved with the addition of new table
   FALSE  ordering not achieved
*/

bool check_join_prefix_contains_ordering(JOIN *join, JOIN_TAB *tab,
                                         table_map previous_tables)
{
  ORDER *order;
  for (order= join->order; order; order= order->next)
  {
    Item *order_item= order->item[0];
    table_map order_tables=order_item->used_tables();
    if (!(order_tables & ~previous_tables) ||
         (order_item->excl_dep_on_table(previous_tables | tab->table->map)))
      continue;
    else
      return FALSE;
  }
  return TRUE;
}


/*
  Checks if it is possible to create a sort nest, if yes
  then it creates the structure for sort-nest
  that includes the number of tables inside the sort-nest
*/

bool create_sort_nest_if_needed(JOIN *join)
{
  uint tablenr, n_tables=0;
  uint table_count= join->table_count;
  for (tablenr=join->const_tables ; tablenr < table_count ; tablenr++)
  {
    POSITION *pos= &join->best_positions[tablenr];
    n_tables++;
    if (pos->sj_strategy == SJ_OPT_MATERIALIZE ||
        pos->sj_strategy == SJ_OPT_MATERIALIZE_SCAN)
    {
      SJ_MATERIALIZATION_INFO *sjm= pos->table->emb_sj_nest->sj_mat_info;
      tablenr+= (sjm->tables-1);
    }
    if (pos->sort_nest_operation_here)
    {
      SORT_NEST_INFO *sort_nest_info;
      if (!(sort_nest_info= new SORT_NEST_INFO()))
        return TRUE;
      sort_nest_info->n_tables= n_tables;
      join->sort_nest_info= sort_nest_info;
      sort_nest_info->index_used= pos->index_no;
      DBUG_ASSERT(sort_nest_info->n_tables != 0);
      return FALSE;
    }
  }
  return FALSE;
}


/*
  Setup the sort-nest struture

  SYNOPSIS

  setup_sort_nest()
    @param join          the join handler

  DESCRIPTION
    Setup execution structures for sort-nest materialization:
    - Create the list of Items that are needed by the sort-nest
    - Create the materialization temporary table for the sort-nest

  @retval
    TRUE   : In case of error
    FALSE  : Nest creation successful
*/

bool setup_sort_nest(JOIN *join)
{

  SORT_NEST_INFO* sort_nest_info= join->sort_nest_info;
  THD *thd= join->thd;
  Field_iterator_table field_iterator;

  JOIN_TAB *start_tab= join->join_tab+join->const_tables, *j, *tab;
  tab= sort_nest_info->nest_tab;
  sort_nest_info->nest_tables_map= 0;

  if (unlikely(thd->trace_started()))
    add_sort_nest_tables_to_trace(join);

  /*
    Here a list of base table items are created that are needed to be stored
    inside the temporary table of the sort-nest. Currently Item_field objects
    are created for the base table fields that are set in the bitmap of
    read_set.
    TODO: in final implementation we can try to remove the fields from this
    list that are completely internal to the nest as they are not needed
    in the post ORDER BY context.
  */

  for (j= start_tab; j < tab; j++)
  {
    sort_nest_info->nest_tables_map|= j->table->map;
    if (j->bush_children)
    {
      TABLE_LIST *emb_sj_nest;
      JOIN_TAB *child_tab= j->bush_children->start;
      emb_sj_nest= child_tab->table->pos_in_table_list->embedding;

      /*
        Walk out of outer join nests until we reach the semi-join
        nest we're in
        Picked from setup_sj_materialization_part1
      */
      while (!emb_sj_nest->sj_mat_info)
        emb_sj_nest= emb_sj_nest->embedding;
      Item_in_subselect *item_sub= emb_sj_nest->sj_subq_pred;
      SELECT_LEX *subq_select= item_sub->unit->first_select();
      List_iterator_fast<Item> li(subq_select->item_list);
      Item *item;
      while((item= li++))
        sort_nest_info->nest_base_table_cols.push_back(item, thd->mem_root);
    }
    else
    {
      TABLE *table= j->table;
      field_iterator.set_table(table);
      for (; !field_iterator.end_of_fields(); field_iterator.next())
      {
        Field *field= field_iterator.field();
        if (!bitmap_is_set(table->read_set, field->field_index))
          continue;
        Item *item;
        if (!(item= field_iterator.create_item(thd)))
          return TRUE;
        sort_nest_info->nest_base_table_cols.push_back(item, thd->mem_root);
      }
    }
  }

  ORDER *order;
  /*
    Substitute the ORDER by items with the best field so that equality
    propagation considered during best_access_path can be used.
  */
  for (order= join->order; order; order=order->next)
  {
    Item *item= order->item[0];
    item= substitute_for_best_equal_field(thd, NO_PARTICULAR_TAB, item,
                                          join->cond_equal,
                                          join->map2table, true);
    item->update_used_tables();
    order->item[0]= item;
  }

  DBUG_ASSERT(!tab->table);

  uint sort_nest_elements= sort_nest_info->nest_base_table_cols.elements;
  sort_nest_info->tmp_table_param.init();
  sort_nest_info->tmp_table_param.bit_fields_as_long= TRUE;
  sort_nest_info->tmp_table_param.field_count= sort_nest_elements;
  sort_nest_info->tmp_table_param.force_not_null_cols= FALSE;

  const LEX_CSTRING order_nest_name= { STRING_WITH_LEN("sort-nest") };
  if (!(tab->table= create_tmp_table(thd, &sort_nest_info->tmp_table_param,
                                     sort_nest_info->nest_base_table_cols,
                                     (ORDER*) 0,
                                     FALSE /* distinct */,
                                     0, /*save_sum_fields*/
                                     thd->variables.option_bits |
                                     TMP_TABLE_ALL_COLUMNS,
                                     HA_POS_ERROR /*rows_limit */,
                                     &order_nest_name)))
    return TRUE; /* purecov: inspected */

  tab->table->map= sort_nest_info->nest_tables_map;
  sort_nest_info->table= tab->table;
  tab->type= JT_ALL;
  tab->table->reginfo.join_tab= tab;

  /*
    The list of temp table items created here, these are needed for the
    substitution for items that would be evaluated in POST SORT NEST context
  */
  field_iterator.set_table(tab->table);
  for (; !field_iterator.end_of_fields(); field_iterator.next())
  {
    Field *field= field_iterator.field();
    Item *item;
    if (!(item= new (thd->mem_root)Item_temptable_field(thd, field)))
      return TRUE;
    sort_nest_info->nest_temp_table_cols.push_back(item, thd->mem_root);
  }

  /*
    Setting up the scan on the temp table
  */
  tab->read_first_record= join_init_read_record;
  tab->read_record.read_record_func= rr_sequential;
  tab[-1].next_select= end_nest_materialization;
  sort_nest_info->materialized= FALSE;

  return FALSE;
}


/*
  Calcualte the cost of adding a sort-nest to the join.

  SYNOPSIS

  sort_nest_oper_cost()
    @param join          the join handler

  @param
    join                the join handler
    join_record_count   the cardinalty of the partial join
    rec_len             length of the record in the sort-nest table

  DESCRIPTION
    The calculation for the cost of the sort-nest is done here, the cost
    included three components
      - Filling the sort-nest table
      - Sorting the sort-nest table
      - Reading from the sort-nest table

*/

double sort_nest_oper_cost(JOIN *join, double join_record_count,
                           ulong rec_len, uint idx)
{
  THD *thd= join->thd;
  double cost= 0;
  /*
    The sort-nest table is not created for sorting when one does sorting
    on the first non-const table. So for this case we don't need to add
    the cost of filling the table.
  */
  if (idx != join->const_tables)
    cost=  get_tmp_table_write_cost(thd, join_record_count,rec_len) *
           join_record_count;   // cost to fill tmp table

  cost+= get_tmp_table_lookup_cost(thd, join_record_count,rec_len) *
         join_record_count;   // cost to perform post join operation used here
  cost+= get_tmp_table_lookup_cost(thd, join_record_count, rec_len) +
         (join_record_count == 0 ? 0 :
          join_record_count * log2 (join_record_count)) *
         SORT_INDEX_CMP_COST;             // cost to perform  sorting
  return cost;
}


double calculate_record_count_for_sort_nest(JOIN *join, uint n_tables)
{
  double sort_nest_records=1, record_count;
  JOIN_TAB *tab= join->join_tab + join->const_tables;
  for (uint j= 0; j < n_tables ;j++, tab++)
  {
    record_count= tab->records_read * tab->cond_selectivity;
    sort_nest_records= COST_MULT(sort_nest_records, record_count);
  }
  sort_nest_records= COST_MULT(sort_nest_records,
                               join->fraction_output_for_nest);
  set_if_bigger(sort_nest_records, 1);
  return sort_nest_records;
}


/*
  @brief
  Find all keys for the table inside join_tab that would satisfy
  the ORDER BY clause
*/

void find_keys_that_can_achieve_ordering(JOIN *join, JOIN_TAB *tab)
{
  if (!join->sort_nest_allowed())
    return;
  TABLE* table= tab->table;
  key_map keys_with_ordering;
  keys_with_ordering.clear_all();
  for (uint index= 0; index < table->s->keys; index++)
  {
    if (table->keys_in_use_for_query.is_set(index) &&
        test_if_order_by_key(join, join->order, table, index))
      keys_with_ordering.set_bit(index);
  }
  table->keys_in_use_for_order_by.intersect(keys_with_ordering);
}

/*
  @brief
  Checks if the partial plan needs filesort for ordering or an index
  picked by best_access_path achieves the ordering

  @retval
    TRUE  : Filesort is needed
    FALSE : index access satifies the ordering
*/

bool needs_filesort(JOIN_TAB *tab, uint idx, int index_used)
{
  JOIN *join= tab->join;
  if (idx && idx == join->const_tables)
    return TRUE;

  TABLE *table= tab->table;
  if (index_used >= 0 && index_used < MAX_KEY)
   return !table->keys_in_use_for_order_by.is_set(index_used);
  return TRUE;
}

/*
  @brief
  Check if an index(used for index scan or range scan) can achieve the
  ordering for the first non-const table, if yes calculate the cost and
  check if we get a better cost than previous access chosen

  @retval
    -1  no cheaper index found for ordering
    >=0 cheaper index found for ordering
*/

int get_best_index_for_order_by_limit(JOIN_TAB *tab, double *read_time,
                                      double *records, double cardinality,
                                      int index_used, uint idx)
{
  TABLE *table= tab->table;
  JOIN *join= tab->join;
  THD *thd= join->thd;
  double save_read_time= *read_time;
  double save_records= *records;
  /**
    Cases when there is no need to consider the indexes that achieve the
    ordering

    1) If there is no limit
    2) If there is no order by clause
    3) Check index for ordering only for the first non-const table
    4) Cardinality is DBL_MAX, then we don't need to consider the index,
       it is sent to DBL_MAX for semi-join strategies
    5) Force index is used
    6) Sort nest is possible and it is not disabled(done when we want to
       get the cardinality)
    7) If there is no index that achieves the ordering
    8) Already an access method is picked that satisfies the ordering

    Do we need to consider non-covering keys that have no range access?
    Currently all indexes that satisfy the ordering are considered
  */
  if (join->select_limit == HA_POS_ERROR ||
      !join->order ||
      idx ||
      cardinality == DBL_MAX ||
      table->force_index ||
      !(join->sort_nest_possible && !join->disable_sort_nest) ||
      table->keys_in_use_for_order_by.is_clear_all())
    return -1;

  Json_writer_object trace_index_for_ordering(thd);
  double est_records= *records;
  double fanout= cardinality / est_records;
  int best_index=-1;
  ha_rows table_records= table->stat_records();
  Json_writer_array considered_indexes(thd, "considered_indexes");
  for (uint idx= 0 ; idx < table->s->keys; idx++)
  {
    if (!table->keys_in_use_for_order_by.is_set(idx))
      continue;
    Json_writer_object possible_key(thd);
    KEY *keyinfo= table->key_info + idx;
        possible_key.add("index", keyinfo->name);
    double rec_per_key, index_scan_time;
    ha_rows select_limit= join->select_limit;
    select_limit= (ha_rows) (select_limit < fanout ?
                             1 : select_limit/fanout);

    est_records= MY_MIN(est_records, ha_rows(table_records *
                                             table->cond_selectivity));
    if (select_limit > est_records)
      select_limit= table_records;
    else
      select_limit= (ha_rows) (select_limit * (double) table_records /
                               est_records);
    possible_key.add("updated_limit", select_limit);
    rec_per_key= keyinfo->actual_rec_per_key(keyinfo->user_defined_key_parts-1);
    set_if_bigger(rec_per_key, 1);
    index_scan_time= select_limit/rec_per_key *
                     MY_MIN(rec_per_key, table->file->scan_time());
    possible_key.add("index_scan_time", index_scan_time);
    double range_scan_time;

    if (get_range_limit_read_cost(tab, table, table_records, idx,
                                  select_limit, &range_scan_time))
    {
      possible_key.add("range_scan_time", range_scan_time);
      if (range_scan_time < index_scan_time)
        index_scan_time= range_scan_time;
    }

    if (index_scan_time < *read_time)
    {
      best_index= idx;
      *read_time= index_scan_time;
      *records= select_limit;
    }
  }
  considered_indexes.end();
  trace_index_for_ordering.add("best_index",
                                static_cast<ulonglong>(best_index));
  trace_index_for_ordering.add("records", *records);
  trace_index_for_ordering.add("best_cost", *read_time);

  /*
    If an index already found satisfied the ordering and we picked index
    scan then revert the cost and stick with the access picked first.
    Index scan would not help in comparision with ref access.
  */
  if (index_satisfies_ordering(tab, index_used))
  {
    if (!table->quick_keys.is_set(static_cast<uint>(index_used)))
    {
      best_index= -1;
      *records= save_records;
      *read_time= save_read_time;
    }
  }
  return best_index;
}


/*
  @brief
   Disable join buffering for all the tables that come after the inner
   table of the sort-nest. This is done because join-buffering does not
   ensure ordering.
*/

bool check_if_join_buffering_needed(JOIN *join, JOIN_TAB *tab)
{
  JOIN_TAB *end= join->join_tab+join->top_join_tab_count;
  JOIN_TAB *start= join->sort_nest_info->nest_tab;
  for (JOIN_TAB *j= start; j < end; j++)
  {
    if (j == tab)
      return FALSE;
  }
  return TRUE;
}


/*
  @brief
  Check if an index satisfies the ORDER BY clause or not

  @retval
    TRUE index satisfies the ordering
    FALSE index does not satisfy the ordering
*/

bool index_satisfies_ordering(JOIN_TAB *tab, int index_used)
{
  TABLE *table= tab->table;
  if (index_used >=0 && index_used < MAX_KEY &&
      !table->keys_in_use_for_order_by.is_clear_all())
  {
    if (table->keys_in_use_for_order_by.is_set(static_cast<uint>(index_used)))
      return TRUE;
  }
  return FALSE;
}


bool setup_range_scan(JOIN *join, JOIN_TAB *tab, uint idx, double records)
{
    SQL_SELECT *sel= NULL;
    Item **sargable_cond= get_sargable_cond(join, tab->table);
    int err, rc, direction;
    uint used_key_parts;
    key_map keymap_for_range;

    /*
      TODO(varun)
      Find a workaround for this, alreay created a QUICK object, can't
      we just use the same object even if the reversing is required.
    */
    if (tab->quick)
    {
      delete tab->quick;
      tab->quick= 0;
    }

    sel= make_select(tab->table, join->const_table_map,
                     join->const_table_map,
                     *sargable_cond, (SORT_INFO*) 0, 1, &err);
    if (!sel)
      goto use_filesort;


    keymap_for_range.clear_all();  // Force the creation of quick select
    keymap_for_range.set_bit(idx); // only for new_ref_key.

    rc= sel->test_quick_select(join->thd, keymap_for_range,
                               (table_map) 0,
                               (ha_rows) HA_POS_ERROR,
                               true, false, true, true);
    if (rc <= 0)
      goto use_filesort;

    direction= test_if_order_by_key(join, join->order, tab->table, idx,
                                    &used_key_parts);
    if (direction == -1)
    {
      QUICK_SELECT_I *reverse_quick;
      if (sel && sel->quick)
      {
        reverse_quick= sel->quick->make_reverse(used_key_parts);
        sel->set_quick(reverse_quick);
      }
    }
    tab->quick= sel->quick;
    /*
      Fix for explain, the records here should be set to the value
      which was stored in the POSITION object as the fraction which we
      would read would have been applied.
    */
    if (records < tab->quick->records)
      tab->quick->records= records;
    sel->quick= 0;

  use_filesort:
    delete sel;
  return rc <=0 ? FALSE : TRUE;

}


/*
  @brief
    Setup range or index scan for the ordering
*/

void setup_index_use_for_ordering(JOIN *join, int index_no)
{
  SORT_NEST_INFO *sort_nest_info= join->sort_nest_info;
  sort_nest_info->nest_tab= join->join_tab + join->const_tables;
  POSITION *cur_pos= &join->best_positions[join->const_tables];
  index_no= (index_no == -1) ?
            (cur_pos->table->quick ? cur_pos->table->quick->index : -1) :
            index_no;
  if (index_satisfies_ordering(cur_pos->table, index_no))
  {
    if (cur_pos->table->table->quick_keys.is_set(index_no))
    {
      // Range scan
      (void)setup_range_scan(join, cur_pos->table, index_no,
                             cur_pos->records_read);
      sort_nest_info->index_used= -1;
    }
    else
    {
      if (cur_pos->table->quick)
      {
        delete cur_pos->table->quick;
        cur_pos->table->quick= 0;
      }
      sort_nest_info->index_used= index_no; // Index scan
    }
  }
  else
    sort_nest_info->index_used= -1;
}


int get_index_on_table(JOIN_TAB *tab)
{
  int idx= -1;
  if (tab->type == JT_REF  || tab->type == JT_EQ_REF ||
      tab->type == JT_REF_OR_NULL)
    idx= tab->ref.key;
  else if (tab->type == JT_NEXT)
    idx= tab->index;
  else if (tab->type == JT_ALL &&
           tab->select && tab->select->quick)
    idx= tab->select->quick->index;

  return idx;
}

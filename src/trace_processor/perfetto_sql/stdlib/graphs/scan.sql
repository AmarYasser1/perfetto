--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

CREATE PERFETTO MACRO _graph_scan_df_agg(x Expr, y Expr)
RETURNS Expr AS __intrinsic_stringify!($x), $y;

CREATE PERFETTO MACRO _graph_scan_bind(x Expr, y Expr)
RETURNS Expr AS __intrinsic_table_ptr_bind($x, __intrinsic_stringify!($y));

CREATE PERFETTO MACRO _graph_scan_select(x Expr, y Expr)
RETURNS Expr AS $x as $y;

-- Performs a "scan" over the grapu starting at `init_table` and using `graph_table`
-- for edges to follow.
--
-- See https://en.wikipedia.org/wiki/Prefix_sum#Scan_higher_order_function for
-- details of what a scan means.
CREATE PERFETTO MACRO _graph_scan(
  -- The table containing the edges of the graph. Needs to have the columns
  -- `source_node_id` and `dest_node_id`.
  graph_table TableOrSubquery,
  -- The table of nodes to start the scan from. Needs to have the column `id`
  -- and all columns specified by `agg_columns`.
  init_table TableOrSubquery,
  -- A paranthesised and comma separated list of columns which will be returned
  -- by the scan. Should match exactly both the names and order of the columns
  -- in `init_table` and `agg_query`.
  --
  -- Example: (cumulative_sum, cumulative_count).
  agg_columns ColumnNameList,
  -- A subquery which aggregates the data for one step of the scan. Should contain
  -- the column `id` and all columns specified by `agg_columns`. Should read from
  -- a variable table labelled `$table`.
  agg_query TableOrSubquery
)
RETURNS TableOrSubquery AS
(
  select
    c0 as id,
    __intrinsic_token_zip_join!(
      (c1, c2, c3, c4, c5, c6, c7),
      $agg_columns,
      _graph_scan_select,
      __intrinsic_token_comma!()
    )
  from  __intrinsic_table_ptr(__intrinsic_graph_scan(
    (
      select __intrinsic_graph_agg(g.source_node_id, g.dest_node_id)
      from $graph_table g
    ),
    (
      select __intrinsic_row_dataframe_agg(
        'id', s.id,
        __intrinsic_token_zip_join!(
          $agg_columns,
          $agg_columns,
          _graph_scan_df_agg,
          __intrinsic_token_comma!()
        )
      )
      from $init_table s
    ),
    __intrinsic_stringify!($agg_query, table),
    __intrinsic_stringify!($agg_columns)
  ))
  where __intrinsic_table_ptr_bind(c0, 'id')
    and __intrinsic_token_zip_join!(
          (c1, c2, c3, c4, c5, c6, c7),
          $agg_columns,
          _graph_scan_bind,
          AND
        )
);

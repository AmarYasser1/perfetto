--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE graphs.hierarchy;

CREATE PERFETTO TABLE _callstack_spf_summary AS
SELECT
  id,
  symbol_set_id,
  (
    SELECT id
    FROM stack_profile_symbol s
    WHERE s.symbol_set_id = f.symbol_set_id
    ORDER BY id
    LIMIT 1
  ) AS min_symbol_id,
  (
    SELECT id
    FROM stack_profile_symbol s
    WHERE s.symbol_set_id = f.symbol_set_id
    ORDER BY id DESC
    LIMIT 1
  ) AS max_symbol_id
FROM stack_profile_frame f
ORDER BY id;

CREATE PERFETTO TABLE _callstack_spc_raw_forest AS
SELECT
  c.id AS callsite_id,
  s.id AS symbol_id,
  IIF(
    s.id IS f.min_symbol_id,
    c.parent_id,
    c.id
  ) AS parent_callsite_id,
  IIF(
    s.id IS f.min_symbol_id,
    pf.max_symbol_id,
    s.id - 1
  ) AS parent_symbol_id,
  f.id AS frame_id,
  s.id IS f.max_symbol_id AS is_leaf
FROM stack_profile_callsite c
JOIN _callstack_spf_summary f ON c.frame_id = f.id
LEFT JOIN stack_profile_symbol s USING (symbol_set_id)
LEFT JOIN stack_profile_callsite p ON c.parent_id = p.id
LEFT JOIN _callstack_spf_summary pf ON p.frame_id = pf.id
ORDER BY c.id;

CREATE PERFETTO TABLE _callstack_spc_forest AS
SELECT
  c._auto_id AS id,
  p._auto_id AS parent_id,
  -- TODO(lalitm): consider demangling in a separate table as
  -- demangling is suprisingly inefficient and is taking a
  -- significant fraction of the runtime on big traces.
  IFNULL(
    DEMANGLE(COALESCE(s.name, f.deobfuscated_name, f.name)),
    COALESCE(s.name, f.deobfuscated_name, f.name)
  ) AS name,
  f.mapping AS mapping_id,
  s.source_file,
  s.line_number,
  c.callsite_id,
  c.is_leaf AS is_leaf_function_in_callsite_frame
FROM _callstack_spc_raw_forest c
JOIN stack_profile_frame f ON c.frame_id = f.id
LEFT JOIN stack_profile_symbol s ON c.symbol_id = s.id
LEFT JOIN _callstack_spc_raw_forest p ON
  p.callsite_id = c.parent_callsite_id
  AND p.symbol_id IS c.parent_symbol_id
ORDER BY c._auto_id;

CREATE PERFETTO MACRO _callstacks_for_stack_profile_samples(
  spc_samples TableOrSubquery
)
RETURNS TableOrSubquery
AS
(
  SELECT
    f.id,
    f.parent_id,
    f.name,
    f.callsite_id,
    f.is_leaf_function_in_callsite_frame
  FROM _tree_reachable_ancestors_or_self!(
    _callstack_spc_forest,
    (SELECT callsite_id AS id FROM $spc_samples)
  ) g
  JOIN _callstack_spc_forest f USING (id)
);

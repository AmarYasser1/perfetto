// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import * as m from 'mithril';

import {Actions} from '../common/actions';
import {Arg, ArgsTree, isArgTreeArray, isArgTreeMap} from '../common/arg_types';
import {timeToCode} from '../common/time';

import {globals, SliceDetails} from './globals';
import {PanelSize} from './panel';
import {verticalScrollToTrack} from './scroll_helper';
import {SlicePanel} from './slice_panel';

// Table row contents is one of two things:
// 1. Key-value pair
interface KVPair {
  kind: 'KVPair';
  key: string;
  value: Arg;
}

// 2. Common prefix for values in an array
interface TableHeader {
  kind: 'TableHeader';
  header: string;
}

type RowContents = KVPair|TableHeader;

function isTableHeader(contents: RowContents): contents is TableHeader {
  return contents.kind === 'TableHeader';
}

function appendPrefix(p1: string, p2: string): string {
  if (p1.length === 0) {
    return p2;
  }
  return `${p1}.${p2}`;
}


// During building the table, sometimes we want to add an extra cell to the
// table. It might be either an index of array element (represented as number),
// a special indentation cell to ensure minimum column width when indenting
// an object ('whitespace' literal) or just be absent ('none' literal).
type ExtraCell = number|'whitespace'|'none';

interface Row {
  // How many columns (empty or with an index) precede a key
  indentLevel: number;
  extraCell: ExtraCell;
  // Optional tooltip to be displayed on the key. Used to display the full key,
  // which has to be reconstructed from the information that might not even be
  // visible on the screen otherwise.
  tooltip?: string;
  contents: RowContents;
}

class TableBuilder {
  stack: ExtraCell[] = [];

  // Row data generated by builder
  rows: Row[] = [];

  // Maximum indent level of a key, used to determine total number of columns
  maxIndent = 0;

  // Add a key-value pair into the table
  add(key: string, value: Arg) {
    this.rows.push({
      indentLevel: 0,
      extraCell: 'none',
      contents: {kind: 'KVPair', key, value},
    });
  }

  // Add arguments tree into the table
  addTree(tree: ArgsTree) {
    this.addTreeInternal(tree, '', '');
  }

  // Return indent level and index for a fresh row
  private prepareRow(): [number, ExtraCell] {
    const level = this.stack.length;
    let index: ExtraCell = 'none';
    if (level > 0) {
      index = this.stack[level - 1];
      if (index !== -1) {
        this.stack[level - 1] = 'none';
      }
    }
    this.maxIndent = Math.max(this.maxIndent, level);
    return [level, index];
  }

  private addTreeInternal(
      record: ArgsTree, prefix: string, completePrefix: string) {
    if (isArgTreeArray(record)) {
      // Add the current prefix as a separate row
      const row = this.prepareRow();
      this.rows.push({
        indentLevel: row[0],
        extraCell: row[1],
        contents: {kind: 'TableHeader', header: prefix},
        tooltip: completePrefix
      });

      for (let i = 0; i < record.length; i++) {
        // Push the current array index to the stack.
        this.stack.push(i);
        // Prefix is empty for array elements because we don't want to repeat
        // the common prefix
        this.addTreeInternal(record[i], '', `${completePrefix}[${i}]`);
        this.stack.pop();
      }
    } else if (isArgTreeMap(record)) {
      const entries = Object.entries(record);
      if (entries.length === 1) {
        // Don't want to create a level of indirection in case object contains
        // only one value; think of it like file browser in IDEs not showing
        // intermediate nodes for common hierarchy corresponding to Java package
        // prefix (e.g. "com/google/perfetto").
        //
        // In this case, add key as a prefix part.
        const [key, value] = entries[0];
        this.addTreeInternal(
            value,
            appendPrefix(prefix, key),
            appendPrefix(completePrefix, key));
      } else {
        if (prefix.length > 0) {
          const row = this.prepareRow();
          this.rows.push({
            indentLevel: row[0],
            extraCell: row[1],
            contents: {kind: 'TableHeader', header: prefix},
            tooltip: completePrefix
          });
          this.stack.push('whitespace');
        }
        for (const [key, value] of entries) {
          this.addTreeInternal(value, key, appendPrefix(completePrefix, key));
        }
        if (prefix.length > 0) {
          this.stack.pop();
        }
      }
    } else {
      // Leaf value in the tree: add to the table
      const row = this.prepareRow();
      this.rows.push({
        indentLevel: row[0],
        extraCell: row[1],
        contents: {kind: 'KVPair', key: prefix, value: record},
        tooltip: completePrefix
      });
    }
  }
}

export class ChromeSliceDetailsPanel extends SlicePanel {
  view() {
    const sliceInfo = globals.sliceDetails;
    if (sliceInfo.ts !== undefined && sliceInfo.dur !== undefined &&
        sliceInfo.name !== undefined) {
      const builder = new TableBuilder();
      builder.add('Name', sliceInfo.name);
      builder.add(
          'Category',
          !sliceInfo.category || sliceInfo.category === '[NULL]' ?
              'N/A' :
              sliceInfo.category);
      builder.add('Start time', timeToCode(sliceInfo.ts));
      builder.add(
          'Duration', this.computeDuration(sliceInfo.ts, sliceInfo.dur));
      if (sliceInfo.thread_ts !== undefined &&
          sliceInfo.thread_dur !== undefined) {
        builder.add(
            'Thread duration',
            this.computeDuration(sliceInfo.thread_ts, sliceInfo.thread_dur));
      }
      builder.add(
          'Slice ID', sliceInfo.id ? sliceInfo.id.toString() : 'Unknown');
      if (sliceInfo.description) {
        this.fillDescription(sliceInfo.description, builder);
      }
      this.fillArgs(sliceInfo, builder);
      return m(
          '.details-panel',
          m('.details-panel-heading', m('h2', `Slice Details`)),
          m('.details-table', this.renderTable(builder)));
    } else {
      return m(
          '.details-panel',
          m('.details-panel-heading',
            m(
                'h2',
                `Slice Details`,
                )));
    }
  }

  renderCanvas(_ctx: CanvasRenderingContext2D, _size: PanelSize) {}

  fillArgs(slice: SliceDetails, builder: TableBuilder) {
    if (slice.argsTree && slice.args) {
      // Parsed arguments are available, need only to iterate over them to get
      // slice references
      for (const [key, value] of slice.args) {
        if (typeof value !== 'string') {
          builder.add(key, value);
        }
      }
      builder.addTree(slice.argsTree);
    } else if (slice.args) {
      // Parsing has failed, but arguments are available: display them in a flat
      // 2-column table
      for (const [key, value] of slice.args) {
        builder.add(key, value);
      }
    }
  }

  renderTable(builder: TableBuilder): m.Vnode {
    const rows: m.Vnode[] = [];
    const keyColumnCount = builder.maxIndent + 1;
    for (const row of builder.rows) {
      const renderedRow: m.Vnode[] = [];
      let indent = row.indentLevel;
      if (row.extraCell !== 'none') {
        indent--;
      }

      if (indent > 0) {
        renderedRow.push(m('td.no-highlight', {colspan: indent}));
      }

      if (row.extraCell === 'whitespace') {
        renderedRow.push(m('td.no-highlight.padding', {class: 'array-index'}));
      } else if (row.extraCell !== 'none') {
        renderedRow.push(m('td', {class: 'array-index'}, `[${row.extraCell}]`));
      }

      if (isTableHeader(row.contents)) {
        renderedRow.push(m(
            'th',
            {colspan: keyColumnCount + 1 - row.indentLevel, title: row.tooltip},
            row.contents.header));
      } else {
        renderedRow.push(
            m('th',
              {colspan: keyColumnCount - row.indentLevel, title: row.tooltip},
              row.contents.key));
        const value = row.contents.value;
        if (typeof value === 'string') {
          renderedRow.push(m('td', value));
        } else {
          // Type of value being a record is not propagated into the callback
          // for some reason, extracting necessary parts as constants instead.
          const sliceId = value.sliceId;
          const trackId = value.trackId;
          renderedRow.push(
              m('td',
                m('i.material-icons.grey',
                  {
                    onclick: () => {
                      globals.makeSelection(Actions.selectChromeSlice(
                          {id: sliceId, trackId, table: 'slice'}));
                      // Ideally we want to have a callback to
                      // findCurrentSelection after this selection has been
                      // made. Here we do not have the info for horizontally
                      // scrolling to ts.
                      verticalScrollToTrack(trackId, true);
                    },
                    title: 'Go to destination slice'
                  },
                  'call_made')));
        }
      }

      rows.push(m('tr', renderedRow));
    }

    return m('table.half-width.auto-layout', rows);
  }

  fillDescription(description: Map<string, string>, builder: TableBuilder) {
    for (const [key, value] of description) {
      builder.add(key, value);
    }
  }
}

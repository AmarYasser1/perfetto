// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';

import {TrackData} from '../../common/track_data';
import {
  Engine,
  LegacyDetailsPanel,
  PERF_SAMPLES_PROFILE_TRACK_KIND,
} from '../../public';
import {LegacyFlamegraphCache} from '../../core/legacy_flamegraph_cache';
import {
  LegacyFlamegraphDetailsPanel,
  profileType,
} from '../../frontend/legacy_flamegraph_panel';
import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {
  LegacySelection,
  PerfSamplesSelection,
} from '../../core/selection_manager';
import {
  QueryFlamegraph,
  QueryFlamegraphAttrs,
  USE_NEW_FLAMEGRAPH_IMPL,
  metricsFromTableOrSubquery,
} from '../../core/query_flamegraph';
import {Monitor} from '../../base/monitor';
import {DetailsShell} from '../../widgets/details_shell';
import {assertExists} from '../../base/logging';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {
  ProcessPerfSamplesProfileTrack,
  ThreadPerfSamplesProfileTrack,
} from './perf_samples_profile_track';
import {getThreadUriPrefix} from '../../public/utils';

export interface Data extends TrackData {
  tsStarts: BigInt64Array;
}

class PerfSamplesProfilePlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const pResult = await ctx.engine.query(`
      select distinct upid
      from perf_sample
      join thread using (utid)
      where callsite_id is not null and upid is not null
    `);
    for (const it = pResult.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      ctx.registerTrack({
        uri: `/process_${upid}/perf_samples_profile`,
        displayName: `Process Callstacks`,
        kind: PERF_SAMPLES_PROFILE_TRACK_KIND,
        upid,
        trackFactory: ({trackKey}) =>
          new ProcessPerfSamplesProfileTrack(
            {
              engine: ctx.engine,
              trackKey,
            },
            upid,
          ),
      });
    }
    const tResult = await ctx.engine.query(`
      select distinct
        utid,
        tid,
        thread.name as threadName,
        upid
      from perf_sample
      join thread using (utid)
      where callsite_id is not null
    `);
    for (
      const it = tResult.iter({
        utid: NUM,
        tid: NUM,
        threadName: STR_NULL,
        upid: NUM_NULL,
      });
      it.valid();
      it.next()
    ) {
      const {threadName, utid, tid, upid} = it;
      const displayName =
        threadName === null
          ? `Thread Callstacks ${tid}`
          : `${threadName} Callstacks ${tid}`;
      ctx.registerTrack({
        uri: `${getThreadUriPrefix(upid, utid)}_perf_samples_profile`,
        displayName,
        kind: PERF_SAMPLES_PROFILE_TRACK_KIND,
        utid,
        trackFactory: ({trackKey}) =>
          new ThreadPerfSamplesProfileTrack(
            {
              engine: ctx.engine,
              trackKey,
            },
            utid,
          ),
      });
    }
    ctx.registerDetailsPanel(new PerfSamplesFlamegraphDetailsPanel(ctx.engine));
  }
}

class PerfSamplesFlamegraphDetailsPanel implements LegacyDetailsPanel {
  private sel?: PerfSamplesSelection;
  private selMonitor = new Monitor([
    () => this.sel?.leftTs,
    () => this.sel?.rightTs,
    () => this.sel?.upid,
    () => this.sel?.type,
  ]);
  private flamegraphAttrs?: QueryFlamegraphAttrs;
  private cache = new LegacyFlamegraphCache('perf_samples');

  constructor(private engine: Engine) {}

  render(sel: LegacySelection) {
    if (sel.kind !== 'PERF_SAMPLES') {
      this.sel = undefined;
      return undefined;
    }
    if (!USE_NEW_FLAMEGRAPH_IMPL.get() && sel.upid !== undefined) {
      this.sel = undefined;
      return m(LegacyFlamegraphDetailsPanel, {
        cache: this.cache,
        selection: {
          profileType: profileType(sel.type),
          start: sel.leftTs,
          end: sel.rightTs,
          upids: [sel.upid],
        },
      });
    }

    const {leftTs, rightTs, upid, utid} = sel;
    this.sel = sel;
    if (this.selMonitor.ifStateChanged()) {
      this.flamegraphAttrs = {
        engine: this.engine,
        metrics: [
          ...metricsFromTableOrSubquery(
            upid === undefined
              ? `
                (
                  select id, parent_id as parentId, name, self_count
                  from _linux_perf_callstacks_for_samples!((
                    select p.callsite_id
                    from perf_sample p
                    where p.ts >= ${leftTs}
                      and p.ts <= ${rightTs}
                      and p.utid = ${utid}
                  ))
                )
              `
              : `
                  (
                    select id, parent_id as parentId, name, self_count
                    from _linux_perf_callstacks_for_samples!((
                      select p.callsite_id
                      from perf_sample p
                      join thread t using (utid)
                      where p.ts >= ${leftTs}
                        and p.ts <= ${rightTs}
                        and t.upid = ${upid}
                    ))
                  )
                `,
            [
              {
                name: 'Perf Samples',
                unit: '',
                columnName: 'self_count',
              },
            ],
            'include perfetto module linux.perf.samples',
          ),
        ],
      };
    }
    return m(
      '.flamegraph-profile',
      m(
        DetailsShell,
        {
          fillParent: true,
          title: m('.title', 'Perf Samples'),
          description: [],
          buttons: [
            m(
              'div.time',
              `First timestamp: `,
              m(Timestamp, {
                ts: this.sel.leftTs,
              }),
            ),
            m(
              'div.time',
              `Last timestamp: `,
              m(Timestamp, {
                ts: this.sel.rightTs,
              }),
            ),
          ],
        },
        m(QueryFlamegraph, assertExists(this.flamegraphAttrs)),
      ),
    );
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.PerfSamplesProfile',
  plugin: PerfSamplesProfilePlugin,
};

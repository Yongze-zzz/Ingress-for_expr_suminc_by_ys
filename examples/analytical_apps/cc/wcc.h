/** Copyright 2020 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef EXAMPLES_ANALYTICAL_APPS_WCC_WCC_H_
#define EXAMPLES_ANALYTICAL_APPS_WCC_WCC_H_

#include <grape/grape.h>

#include "cc/wcc_context.h"

namespace grape {

/**
 * @brief WCC application, determines the weakly connected component each vertex
 * belongs to, which only works on both undirected graph.
 *
 * This version of WCC inherits ParallelAppBase. Messages can be sent in
 * parallel to the evaluation. This strategy improve performance by overlapping
 * the communication time and the evaluation time.
 *
 * @tparam FRAG_T
 */
template <typename FRAG_T>
class WCC : public ParallelAppBase<FRAG_T, WCCContext<FRAG_T>>,
            public ParallelEngine {
  INSTALL_PARALLEL_WORKER(WCC<FRAG_T>, WCCContext<FRAG_T>, FRAG_T)
  using vertex_t = typename fragment_t::vertex_t;
  using vid_t = typename fragment_t::vid_t;

  static constexpr bool need_split_edges = true;

 private:
  // Propagate label through pushing
  // Each vertex pushes its state to update neighbors.
  void PropagateLabelPush(const fragment_t& frag, context_t& ctx,
                          message_manager_t& messages) {
    auto inner_vertices = frag.InnerVertices();
    auto outer_vertices = frag.OuterVertices();

    // propagate label to incoming and outgoing neighbors
    ForEach(ctx.curr_modified, inner_vertices,
            [&frag, &ctx](int tid, vertex_t v) {
              auto cid = ctx.comp_id[v];
              auto es = frag.GetOutgoingAdjList(v);
              for (auto& e : es) {
                auto u = e.neighbor;
                if (ctx.comp_id[u] > cid) {
                  atomic_min(ctx.comp_id[u], cid);
                  ctx.next_modified.Insert(u);
                }
              }
            });

    ForEach(outer_vertices, [&messages, &frag, &ctx](int tid, vertex_t v) {
      if (ctx.next_modified.Exist(v)) {
        messages.SyncStateOnOuterVertex<fragment_t, vid_t>(frag, v,
                                                           ctx.comp_id[v], tid);
      }
    });
  }

 public:
  void PEval(const fragment_t& frag, context_t& ctx,
             message_manager_t& messages) {
    auto inner_vertices = frag.InnerVertices();
    auto outer_vertices = frag.OuterVertices();

    messages.InitChannels(thread_num());

    // assign initial component id with global id
    ForEach(inner_vertices, [&frag, &ctx](int tid, vertex_t v) {
      ctx.comp_id[v] = frag.GetInnerVertexGid(v);
      ctx.curr_modified.Insert(v);
    });
    ForEach(outer_vertices, [&frag, &ctx](int tid, vertex_t v) {
      ctx.comp_id[v] = frag.GetOuterVertexGid(v);
    });

    PropagateLabelPush(frag, ctx, messages);

    if (!ctx.next_modified.PartialEmpty(0, frag.GetInnerVerticesNum())) {
      messages.ForceContinue();
    }

    ctx.curr_modified.Swap(ctx.next_modified);
  }

  void IncEval(const fragment_t& frag, context_t& ctx,
               message_manager_t& messages) {
    ctx.next_modified.ParallelClear(thread_num());

    // aggregate messages
    messages.ParallelProcess<fragment_t, vid_t>(
        thread_num(), frag, [&ctx](int tid, vertex_t u, vid_t msg) {
          if (ctx.comp_id[u] > msg) {
            atomic_min(ctx.comp_id[u], msg);
            ctx.curr_modified.Insert(u);
          }
        });

    PropagateLabelPush(frag, ctx, messages);

    if (!ctx.next_modified.PartialEmpty(0, frag.GetInnerVerticesNum())) {
      messages.ForceContinue();
    }

    ctx.curr_modified.Swap(ctx.next_modified);
  }
};

}  // namespace grape
#endif  // EXAMPLES_ANALYTICAL_APPS_WCC_WCC_H_

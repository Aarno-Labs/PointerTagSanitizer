; RUN: not --crash %opt_ptsan -ptsan-nochecks -ptsan-stop-after=late-optimization %s 2>&1 | FileCheck %s

; Tag anchors are created by min-cut rewriting. Seeing one while building the
; graph means an earlier phase violated that boundary.
;
; CHECK: tag anchors must be created by min-cut rewriting

declare ptr @__ptsan_tag(ptr, i64)

define ptr @tag_before_min_cut(ptr %p, i64 %id) {
  %tagged = call ptr @__ptsan_tag(ptr %p, i64 %id)
  ret ptr %tagged
}

target triple = "x86_64-unknown-linux-gnu"

define dso_local i32 @ptr_gt_m1(ptr noundef %p) local_unnamed_addr {
entry:
  %cmp = icmp sgt ptr %p, inttoptr (i64 -1 to ptr)
  %res = zext i1 %cmp to i32
  ret i32 %res
}

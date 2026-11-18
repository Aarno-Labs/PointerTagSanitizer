// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefixes=STRONG,STRONG-DEFAULT
// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -mllvm -ptsan-stack-temporal-safety=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=WEAK

// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O1 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefixes=STRONG,STRONG-ID32 --implicit-check-not=ptsan_pending_stack_ids --implicit-check-not=llvm.memset --implicit-check-not=fence

// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -mllvm -ptsan-32-bit-ids-origin-quantum=65536 -O1 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefix=SLOW --implicit-check-not=has.stack.id.room --implicit-check-not='call i32 @ptsan_refill_stack_origins'

extern void escape(char *);

__attribute__((noinline))
void one_stack_id(void) {
  char buf[17];
  escape(buf);
}

// STRONG-LABEL: define {{.*}}@one_stack_id(
// STRONG-DEFAULT: pending.frees
// STRONG-DEFAULT: pending.frees.inc
// STRONG-DEFAULT: call void @llvm.memset
// STRONG-ID32: load ptr, ptr @ptsan_stack_origin_next
// STRONG-ID32: [[SALT:%.*]] = load i32, ptr
// STRONG-ID32: icmp {{eq|ne}} i32 [[SALT]], 0
// STRONG-ID32: store ptr {{.*}}, ptr @ptsan_stack_origin_next
// STRONG-ID32: call i32 @ptsan_refill_stack_origins()
// STRONG-ID32: call void @ptsan_get_stack_ids
// STRONG-ID32: call i32 @ptsan_choose_stack_origin(i64 4096)
// STRONG-ID32: call void @escape
// STRONG-ID32: [[SPAN:%.*]] = getelementptr inbounds{{( nuw)?}} i8, ptr {{.*}}, i64 8
// STRONG-ID32-NEXT: store i64 0, ptr [[SPAN]]
// STRONG-ID32: store i64 {{.*}}, ptr @ptsan_next_stack_id
// STRONG-ID32: call void @ptsan_free_stack_ids
// STRONG: ret void

// WEAK-LABEL: define {{.*}}@one_stack_id(
// WEAK-NOT: pending.frees
// WEAK-NOT: ptsan_pending_stack_ids
// WEAK-NOT: @llvm.memset
// WEAK: store i64 {{.*}}, ptr @ptsan_next_stack_id
// WEAK: ret void

// SLOW-LABEL: define {{.*}}@one_stack_id(
// SLOW: call void @ptsan_get_stack_ids
// SLOW: call i32 @ptsan_choose_stack_origin(i64 65536)
// SLOW: call void @escape
// SLOW: call void @ptsan_free_stack_ids
// SLOW: ret void

(module
   (import "env" "kv_set" (func $kv_set (param i32 i64 i32 i32 i32 i32) (result i64)))
   (memory $0 528)
   (export "apply" (func $apply))
   (func $apply (param $receiver i64) (param $account i64) (param $action_name i64)
      (local $i i32)
      ;; Store ~33MB across 132 rows of 262000 bytes each (just under max_kv_value_size 262144).
      ;; Key = 4 bytes at memory offset 0 (stores the loop counter).
      ;; Value = 262000 bytes at memory offset 4.
      ;; Total: 132 * 262000 = 34,584,000 bytes (~33MB).
      (set_local $i (i32.const 0))
      (block $break
         (loop $loop
            (br_if $break (i32.ge_u (get_local $i) (i32.const 132)))
            ;; Write loop counter to memory[0..4] as the key
            (i32.store (i32.const 0) (get_local $i))
            ;; kv_set(table_id=0, payer=0, key_ptr=0, key_size=4, value_ptr=4, value_size=262000)
            (drop (call $kv_set (i32.const 0) (i64.const 0) (i32.const 0) (i32.const 4) (i32.const 4) (i32.const 262000)))
            (set_local $i (i32.add (get_local $i) (i32.const 1)))
            (br $loop)
         )
      )
   )
)

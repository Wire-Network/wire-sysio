(module
  (type (;0;) (func (param i32 i32)))
  (type (;1;) (func (param i64) (result i32)))
  (type (;2;) (func (param i32 i32 i32) (result i32)))
  (type (;3;) (func (result i32)))
  (type (;4;) (func (param i32 i32) (result i32)))
  (type (;5;) (func))
  (type (;6;) (func (param i32) (result i32)))
  (type (;7;) (func (param i32)))
  (type (;8;) (func (param i64 i64 i64)))
  (type (;9;) (func (result i64)))
  (import "env" "sysio_assert" (func (;0;) (type 0)))
  (import "env" "is_account" (func (;1;) (type 1)))
  (import "env" "memset" (func (;2;) (type 2)))
  (import "env" "action_data_size" (func (;3;) (type 3)))
  (import "env" "read_action_data" (func (;4;) (type 4)))
  (import "env" "memcpy" (func (;5;) (type 2)))
  (func (;6;) (type 5)
    call 8)
  (func (;7;) (type 6) (param i32) (result i32)
    (local i32 i32 i32)
    block  ;; label = @1
      block  ;; label = @2
        block  ;; label = @3
          block  ;; label = @4
            get_local 0
            i32.eqz
            br_if 0 (;@4;)
            i32.const 0
            i32.const 0
            i32.load offset=8204
            get_local 0
            i32.const 16
            i32.shr_u
            tee_local 1
            i32.add
            tee_local 2
            i32.store offset=8204
            i32.const 0
            i32.const 0
            i32.load offset=8196
            tee_local 3
            get_local 0
            i32.add
            i32.const 7
            i32.add
            i32.const -8
            i32.and
            tee_local 0
            i32.store offset=8196
            get_local 2
            i32.const 16
            i32.shl
            get_local 0
            i32.le_u
            br_if 1 (;@3;)
            get_local 1
            memory.grow
            i32.const -1
            i32.eq
            br_if 2 (;@2;)
            br 3 (;@1;)
          end
          i32.const 0
          return
        end
        i32.const 0
        get_local 2
        i32.const 1
        i32.add
        i32.store offset=8204
        get_local 1
        i32.const 1
        i32.add
        memory.grow
        i32.const -1
        i32.ne
        br_if 1 (;@1;)
      end
      i32.const 0
      i32.const 8208
      call 0
      get_local 3
      return
    end
    get_local 3)
  (func (;8;) (type 5)
    (local i32)
    get_global 0
    i32.const 16
    i32.sub
    tee_local 0
    i32.const 0
    i32.store offset=12
    i32.const 0
    get_local 0
    i32.load offset=12
    i32.load
    i32.const 7
    i32.add
    i32.const -8
    i32.and
    tee_local 0
    i32.store offset=8196
    i32.const 0
    get_local 0
    i32.store offset=8192
    i32.const 0
    memory.size
    i32.store offset=8204)
  (func (;9;) (type 7) (param i32))
  (func (;10;) (type 8) (param i64 i64 i64)
    (local i32)
    get_global 0
    i32.const 16
    i32.sub
    tee_local 3
    set_global 0
    call 6
    block  ;; label = @1
      get_local 0
      get_local 1
      i64.eq
      br_if 0 (;@1;)
      i32.const 0
      i32.const 8233
      call 0
    end
    block  ;; label = @1
      block  ;; label = @2
        get_local 1
        i64.const 14389258095169634304
        i64.ne
        br_if 0 (;@2;)
        block  ;; label = @3
          get_local 2
          i64.const -7297973096368160768
          i64.eq
          br_if 0 (;@3;)
          get_local 2
          i64.const -4417316391904870400
          i64.ne
          br_if 1 (;@2;)
          call 11
          i64.const 14389258095169634304
          i64.ne
          br_if 1 (;@2;)
          i64.const -4999377783415635968
          call 1
          br_if 1 (;@2;)
          br 2 (;@1;)
        end
        get_local 3
        call 12
        get_local 3
        i64.load offset=8
        i64.const -4999377783415635968
        i64.eq
        br_if 1 (;@1;)
      end
      i32.const 0
      i32.const 8261
      call 0
    end
    i32.const 0
    call 9
    get_local 3
    i32.const 16
    i32.add
    set_global 0)
  (func (;11;) (type 9) (result i64)
    (local i32 i32 i32 i64)
    get_global 0
    i32.const 16
    i32.sub
    tee_local 0
    set_local 1
    get_local 0
    set_global 0
    block  ;; label = @1
      block  ;; label = @2
        call 3
        tee_local 2
        i32.const 513
        i32.lt_u
        br_if 0 (;@2;)
        get_local 2
        call 7
        set_local 0
        br 1 (;@1;)
      end
      get_local 0
      get_local 2
      i32.const 15
      i32.add
      i32.const -16
      i32.and
      i32.sub
      tee_local 0
      set_global 0
    end
    get_local 0
    get_local 2
    call 4
    drop
    get_local 1
    i64.const 0
    i64.store offset=8
    block  ;; label = @1
      get_local 2
      i32.const 7
      i32.gt_u
      br_if 0 (;@1;)
      i32.const 0
      i32.const 8283
      call 0
    end
    get_local 1
    i32.const 8
    i32.add
    get_local 0
    i32.const 8
    call 5
    drop
    get_local 1
    i64.load offset=8
    set_local 3
    get_local 1
    i32.const 16
    i32.add
    set_global 0
    get_local 3)
  (func (;12;) (type 7) (param i32)
    (local i32 i32 i32)
    get_global 0
    i32.const 16
    i32.sub
    tee_local 1
    set_local 2
    get_local 1
    set_global 0
    block  ;; label = @1
      block  ;; label = @2
        call 3
        tee_local 3
        i32.const 513
        i32.lt_u
        br_if 0 (;@2;)
        get_local 3
        call 7
        set_local 1
        br 1 (;@1;)
      end
      get_local 1
      get_local 3
      i32.const 15
      i32.add
      i32.const -16
      i32.and
      i32.sub
      tee_local 1
      set_global 0
    end
    get_local 1
    get_local 3
    call 4
    drop
    get_local 0
    i64.const 0
    i64.store offset=8
    get_local 0
    i64.const 0
    i64.store
    get_local 2
    i64.const 0
    i64.store offset=8
    get_local 2
    i64.const 0
    i64.store
    block  ;; label = @1
      get_local 3
      i32.const 7
      i32.gt_u
      br_if 0 (;@1;)
      i32.const 0
      i32.const 8283
      call 0
    end
    get_local 2
    i32.const 8
    i32.add
    get_local 1
    i32.const 8
    call 5
    drop
    get_local 1
    i32.const 8
    i32.add
    set_local 1
    block  ;; label = @1
      get_local 3
      i32.const -8
      i32.and
      i32.const 8
      i32.ne
      br_if 0 (;@1;)
      i32.const 0
      i32.const 8283
      call 0
    end
    get_local 2
    get_local 1
    i32.const 8
    call 5
    drop
    get_local 0
    get_local 2
    i64.load offset=8
    i64.store
    get_local 0
    i32.const 8
    i32.add
    get_local 2
    i64.load
    i64.store
    get_local 2
    i32.const 16
    i32.add
    set_global 0)
  (table (;0;) 1 1 anyfunc)
  (memory (;0;) 1)
  (global (;0;) (mut i32) (i32.const 8192))
  (global (;1;) i32 (i32.const 8288))
  (global (;2;) i32 (i32.const 8288))
  (export "apply" (func 10))
  (data (i32.const 8208) "failed to allocate pages\00rejecting all notifications\00")
  (data (i32.const 8261) "rejecting all actions\00")
  (data (i32.const 8283) "read\00")
  (data (i32.const 0) "` \00\00"))

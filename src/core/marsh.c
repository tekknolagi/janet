/*
* Copyright (c) 2019 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#ifndef JANET_AMALG
#include <janet.h>
#include "state.h"
#include "vector.h"
#include "gc.h"
#include "fiber.h"
#include "util.h"
#endif

typedef struct {
    JanetBuffer *buf;
    JanetTable seen;
    JanetTable *rreg;
    JanetFuncEnv **seen_envs;
    JanetFuncDef **seen_defs;
    int32_t nextid;
} MarshalState;

/* Lead bytes in marshaling protocol */
enum {
    LB_REAL = 200,
    LB_NIL,
    LB_FALSE,
    LB_TRUE,
    LB_FIBER,
    LB_INTEGER,
    LB_STRING,
    LB_SYMBOL,
    LB_KEYWORD,
    LB_ARRAY,
    LB_TUPLE,
    LB_TABLE,
    LB_TABLE_PROTO,
    LB_STRUCT,
    LB_BUFFER,
    LB_FUNCTION,
    LB_REGISTRY,
    LB_ABSTRACT,
    LB_REFERENCE,
    LB_FUNCENV_REF,
    LB_FUNCDEF_REF
} LeadBytes;

/* Helper to look inside an entry in an environment */
static Janet entry_getval(Janet env_entry) {
    if (janet_checktype(env_entry, JANET_TABLE)) {
        JanetTable *entry = janet_unwrap_table(env_entry);
        Janet checkval = janet_table_get(entry, janet_ckeywordv("value"));
        if (janet_checktype(checkval, JANET_NIL)) {
            checkval = janet_table_get(entry, janet_ckeywordv("ref"));
        }
        return checkval;
    } else if (janet_checktype(env_entry, JANET_STRUCT)) {
        const JanetKV *entry = janet_unwrap_struct(env_entry);
        Janet checkval = janet_struct_get(entry, janet_ckeywordv("value"));
        if (janet_checktype(checkval, JANET_NIL)) {
            checkval = janet_struct_get(entry, janet_ckeywordv("ref"));
        }
        return checkval;
    } else {
        return janet_wrap_nil();
    }
}

/* Make a forward lookup table from an environment (for unmarshaling) */
JanetTable *janet_env_lookup(JanetTable *env) {
    JanetTable *renv = janet_table(env->count);
    while (env) {
        for (int32_t i = 0; i < env->capacity; i++) {
            if (janet_checktype(env->data[i].key, JANET_SYMBOL)) {
                janet_table_put(renv,
                                env->data[i].key,
                                entry_getval(env->data[i].value));
            }
        }
        env = env->proto;
    }
    return renv;
}

/* Marshal an integer onto the buffer */
static void pushint(MarshalState *st, int32_t x) {
    if (x >= 0 && x < 128) {
        janet_buffer_push_u8(st->buf, x);
    } else if (x <= 8191 && x >= -8192) {
        uint8_t intbuf[2];
        intbuf[0] = ((x >> 8) & 0x3F) | 0x80;
        intbuf[1] = x & 0xFF;
        janet_buffer_push_bytes(st->buf, intbuf, 2);
    } else {
        uint8_t intbuf[5];
        intbuf[0] = LB_INTEGER;
        intbuf[1] = (x >> 24) & 0xFF;
        intbuf[2] = (x >> 16) & 0xFF;
        intbuf[3] = (x >> 8) & 0xFF;
        intbuf[4] = x & 0xFF;
        janet_buffer_push_bytes(st->buf, intbuf, 5);
    }
}

static void pushbyte(MarshalState *st, uint8_t b) {
    janet_buffer_push_u8(st->buf, b);
}

static void pushbytes(MarshalState *st, const uint8_t *bytes, int32_t len) {
    janet_buffer_push_bytes(st->buf, bytes, len);
}

/* Forward declaration to enable mutual recursion. */
static void marshal_one(MarshalState *st, Janet x, int flags);
static void marshal_one_fiber(MarshalState *st, JanetFiber *fiber, int flags);
static void marshal_one_def(MarshalState *st, JanetFuncDef *def, int flags);
static void marshal_one_env(MarshalState *st, JanetFuncEnv *env, int flags);

/* Prevent stack overflows */
#define MARSH_STACKCHECK if ((flags & 0xFFFF) > JANET_RECURSION_GUARD) janet_panic("stack overflow")

/* Marshal a function env */
static void marshal_one_env(MarshalState *st, JanetFuncEnv *env, int flags) {
    MARSH_STACKCHECK;
    for (int32_t i = 0; i < janet_v_count(st->seen_envs); i++) {
        if (st->seen_envs[i] == env) {
            pushbyte(st, LB_FUNCENV_REF);
            pushint(st, i);
            return;
        }
    }
    janet_v_push(st->seen_envs, env);
    pushint(st, env->offset);
    pushint(st, env->length);
    if (env->offset) {
        /* On stack variant */
        marshal_one(st, janet_wrap_fiber(env->as.fiber), flags + 1);
    } else {
        /* Off stack variant */
        for (int32_t i = 0; i < env->length; i++)
            marshal_one(st, env->as.values[i], flags + 1);
    }
}

/* Add function flags to janet functions */
static void janet_func_addflags(JanetFuncDef *def) {
    if (def->name) def->flags |= JANET_FUNCDEF_FLAG_HASNAME;
    if (def->source) def->flags |= JANET_FUNCDEF_FLAG_HASSOURCE;
    if (def->defs) def->flags |= JANET_FUNCDEF_FLAG_HASDEFS;
    if (def->environments) def->flags |= JANET_FUNCDEF_FLAG_HASENVS;
    if (def->sourcemap) def->flags |= JANET_FUNCDEF_FLAG_HASSOURCEMAP;
}

/* Marshal a function def */
static void marshal_one_def(MarshalState *st, JanetFuncDef *def, int flags) {
    MARSH_STACKCHECK;
    for (int32_t i = 0; i < janet_v_count(st->seen_defs); i++) {
        if (st->seen_defs[i] == def) {
            pushbyte(st, LB_FUNCDEF_REF);
            pushint(st, i);
            return;
        }
    }
    janet_func_addflags(def);
    /* Add to lookup */
    janet_v_push(st->seen_defs, def);
    pushint(st, def->flags);
    pushint(st, def->slotcount);
    pushint(st, def->arity);
    pushint(st, def->constants_length);
    pushint(st, def->bytecode_length);
    if (def->flags & JANET_FUNCDEF_FLAG_HASENVS)
        pushint(st, def->environments_length);
    if (def->flags & JANET_FUNCDEF_FLAG_HASDEFS)
        pushint(st, def->defs_length);
    if (def->flags & JANET_FUNCDEF_FLAG_HASNAME)
        marshal_one(st, janet_wrap_string(def->name), flags);
    if (def->flags & JANET_FUNCDEF_FLAG_HASSOURCE)
        marshal_one(st, janet_wrap_string(def->source), flags);

    /* marshal constants */
    for (int32_t i = 0; i < def->constants_length; i++)
        marshal_one(st, def->constants[i], flags);

    /* marshal the bytecode */
    for (int32_t i = 0; i < def->bytecode_length; i++) {
        pushbyte(st, def->bytecode[i] & 0xFF);
        pushbyte(st, (def->bytecode[i] >> 8) & 0xFF);
        pushbyte(st, (def->bytecode[i] >> 16) & 0xFF);
        pushbyte(st, (def->bytecode[i] >> 24) & 0xFF);
    }

    /* marshal the environments if needed */
    for (int32_t i = 0; i < def->environments_length; i++)
        pushint(st, def->environments[i]);

    /* marshal the sub funcdefs if needed */
    for (int32_t i = 0; i < def->defs_length; i++)
        marshal_one_def(st, def->defs[i], flags);

    /* marshal source maps if needed */
    if (def->flags & JANET_FUNCDEF_FLAG_HASSOURCEMAP) {
        int32_t current = 0;
        for (int32_t i = 0; i < def->bytecode_length; i++) {
            JanetSourceMapping map = def->sourcemap[i];
            pushint(st, map.start - current);
            pushint(st, map.end - map.start);
            current = map.end;
        }
    }
}

#define JANET_FIBER_FLAG_HASCHILD (1 << 29)
#define JANET_STACKFRAME_HASENV (1 << 30)

/* Marshal a fiber */
static void marshal_one_fiber(MarshalState *st, JanetFiber *fiber, int flags) {
    MARSH_STACKCHECK;
    int32_t fflags = fiber->flags;
    if (fiber->child) fflags |= JANET_FIBER_FLAG_HASCHILD;
    if (janet_fiber_status(fiber) == JANET_STATUS_ALIVE)
        janet_panic("cannot marshal alive fiber");
    pushint(st, fflags);
    pushint(st, fiber->frame);
    pushint(st, fiber->stackstart);
    pushint(st, fiber->stacktop);
    pushint(st, fiber->maxstack);
    /* Do frames */
    int32_t i = fiber->frame;
    int32_t j = fiber->stackstart - JANET_FRAME_SIZE;
    while (i > 0) {
        JanetStackFrame *frame = (JanetStackFrame *)(fiber->data + i - JANET_FRAME_SIZE);
        if (frame->env) frame->flags |= JANET_STACKFRAME_HASENV;
        if (!frame->func) janet_panic("cannot marshal fiber with c stackframe");
        pushint(st, frame->flags);
        pushint(st, frame->prevframe);
        int32_t pcdiff = (int32_t)(frame->pc - frame->func->def->bytecode);
        pushint(st, pcdiff);
        marshal_one(st, janet_wrap_function(frame->func), flags + 1);
        if (frame->env) marshal_one_env(st, frame->env, flags + 1);
        /* Marshal all values in the stack frame */
        for (int32_t k = i; k < j; k++)
            marshal_one(st, fiber->data[k], flags + 1);
        j = i - JANET_FRAME_SIZE;
        i = frame->prevframe;
    }
    if (fiber->child)
        marshal_one(st, janet_wrap_fiber(fiber->child), flags + 1);
}

void janet_marshal_int(JanetMarshalContext *ctx, int32_t value) {
    MarshalState *st = (MarshalState *)(ctx->m_state);
    pushint(st, value);
};

void janet_marshal_byte(JanetMarshalContext *ctx, uint8_t value) {
    MarshalState *st = (MarshalState *)(ctx->m_state);
    pushbyte(st, value);
};

void janet_marshal_bytes(JanetMarshalContext *ctx, const uint8_t *bytes, int32_t len) {
    MarshalState *st = (MarshalState *)(ctx->m_state);
    pushbytes(st, bytes, len);
}

void janet_marshal_janet(JanetMarshalContext *ctx, Janet x) {
    MarshalState *st = (MarshalState *)(ctx->m_state);
    marshal_one(st, x, ctx->flags + 1);
}

#define MARK_SEEN() \
    janet_table_put(&st->seen, x, janet_wrap_integer(st->nextid++))

static void marshal_one_abstract(MarshalState *st, Janet x, int flags) {
    void *abstract = janet_unwrap_abstract(x);
    const JanetAbstractType *at = janet_abstract_type(abstract);
    if (at->marshal) {
        MARK_SEEN();
        JanetMarshalContext context = {st, NULL, flags, NULL};
        pushbyte(st, LB_ABSTRACT);
        marshal_one(st, janet_ckeywordv(at->name), flags + 1);
        pushint(st, janet_abstract_size(abstract));
        at->marshal(abstract, &context);
    } else {
        janet_panicf("try to marshal unregistered abstract type, cannot marshal %p", x);
    }
}

/* The main body of the marshaling function. Is the main
 * entry point for the mutually recursive functions. */
static void marshal_one(MarshalState *st, Janet x, int flags) {
    MARSH_STACKCHECK;
    JanetType type = janet_type(x);

    /* Check simple primitives (non reference types, no benefit from memoization) */
    switch (type) {
        default:
            break;
        case JANET_NIL:
        case JANET_FALSE:
        case JANET_TRUE:
            pushbyte(st, 200 + type);
            return;
        case JANET_NUMBER: {
            double xval = janet_unwrap_number(x);
            if (janet_checkintrange(xval)) {
                pushint(st, (int32_t) xval);
                return;
            }
            break;
        }
    }


    /* Check reference and registry value */
    {
        Janet check = janet_table_get(&st->seen, x);
        if (janet_checkint(check)) {
            pushbyte(st, LB_REFERENCE);
            pushint(st, janet_unwrap_integer(check));
            return;
        }
        if (st->rreg) {
            check = janet_table_get(st->rreg, x);
            if (janet_checktype(check, JANET_SYMBOL)) {
                MARK_SEEN();
                const uint8_t *regname = janet_unwrap_symbol(check);
                pushbyte(st, LB_REGISTRY);
                pushint(st, janet_string_length(regname));
                pushbytes(st, regname, janet_string_length(regname));
                return;
            }
        }
    }

    /* Reference types */
    switch (type) {
        case JANET_NUMBER: {
            union {
                double d;
                uint8_t bytes[8];
            } u;
            u.d = janet_unwrap_number(x);
#ifdef JANET_BIG_ENDIAN
            /* Swap byte order */
            uint8_t temp;
            temp = u.bytes[7];
            u.bytes[7] = u.bytes[0];
            u.bytes[0] = temp;
            temp = u.bytes[6];
            u.bytes[6] = u.bytes[1];
            u.bytes[1] = temp;
            temp = u.bytes[5];
            u.bytes[5] = u.bytes[2];
            u.bytes[2] = temp;
            temp = u.bytes[4];
            u.bytes[4] = u.bytes[3];
            u.bytes[3] = temp;
#endif
            pushbyte(st, LB_REAL);
            pushbytes(st, u.bytes, 8);
            MARK_SEEN();
            return;
        }
        case JANET_STRING:
        case JANET_SYMBOL:
        case JANET_KEYWORD: {
            const uint8_t *str = janet_unwrap_string(x);
            int32_t length = janet_string_length(str);
            /* Record reference */
            MARK_SEEN();
            uint8_t lb = (type == JANET_STRING) ? LB_STRING :
                         (type == JANET_SYMBOL) ? LB_SYMBOL :
                         LB_KEYWORD;
            pushbyte(st, lb);
            pushint(st, length);
            pushbytes(st, str, length);
            return;
        }
        case JANET_BUFFER: {
            JanetBuffer *buffer = janet_unwrap_buffer(x);
            /* Record reference */
            MARK_SEEN();
            pushbyte(st, LB_BUFFER);
            pushint(st, buffer->count);
            pushbytes(st, buffer->data, buffer->count);
            return;
        }
        case JANET_ARRAY: {
            int32_t i;
            JanetArray *a = janet_unwrap_array(x);
            MARK_SEEN();
            pushbyte(st, LB_ARRAY);
            pushint(st, a->count);
            for (i = 0; i < a->count; i++)
                marshal_one(st, a->data[i], flags + 1);
            return;
        }
        case JANET_TUPLE: {
            int32_t i, count, flag;
            const Janet *tup = janet_unwrap_tuple(x);
            count = janet_tuple_length(tup);
            flag = janet_tuple_flag(tup) >> 16;
            pushbyte(st, LB_TUPLE);
            pushint(st, count);
            pushint(st, flag);
            for (i = 0; i < count; i++)
                marshal_one(st, tup[i], flags + 1);
            /* Mark as seen AFTER marshaling */
            MARK_SEEN();
            return;
        }
        case JANET_TABLE: {
            JanetTable *t = janet_unwrap_table(x);
            MARK_SEEN();
            pushbyte(st, t->proto ? LB_TABLE_PROTO : LB_TABLE);
            pushint(st, t->count);
            if (t->proto)
                marshal_one(st, janet_wrap_table(t->proto), flags + 1);
            for (int32_t i = 0; i < t->capacity; i++) {
                if (janet_checktype(t->data[i].key, JANET_NIL))
                    continue;
                marshal_one(st, t->data[i].key, flags + 1);
                marshal_one(st, t->data[i].value, flags + 1);
            }
            return;
        }
        case JANET_STRUCT: {
            int32_t count;
            const JanetKV *struct_ = janet_unwrap_struct(x);
            count = janet_struct_length(struct_);
            pushbyte(st, LB_STRUCT);
            pushint(st, count);
            for (int32_t i = 0; i < janet_struct_capacity(struct_); i++) {
                if (janet_checktype(struct_[i].key, JANET_NIL))
                    continue;
                marshal_one(st, struct_[i].key, flags + 1);
                marshal_one(st, struct_[i].value, flags + 1);
            }
            /* Mark as seen AFTER marshaling */
            MARK_SEEN();
            return;
        }
        case JANET_ABSTRACT: {
            marshal_one_abstract(st, x, flags);
            return;
        }
        case JANET_FUNCTION: {
            pushbyte(st, LB_FUNCTION);
            JanetFunction *func = janet_unwrap_function(x);
            marshal_one_def(st, func->def, flags);
            /* Mark seen after reading def, but before envs */
            MARK_SEEN();
            for (int32_t i = 0; i < func->def->environments_length; i++)
                marshal_one_env(st, func->envs[i], flags + 1);
            return;
        }
        case JANET_FIBER: {
            MARK_SEEN();
            pushbyte(st, LB_FIBER);
            marshal_one_fiber(st, janet_unwrap_fiber(x), flags + 1);
            return;
        }
        default: {
            janet_panicf("no registry value and cannot marshal %p", x);
            return;
        }
    }
#undef MARK_SEEN
}

void janet_marshal(
    JanetBuffer *buf,
    Janet x,
    JanetTable *rreg,
    int flags) {
    MarshalState st;
    st.buf = buf;
    st.nextid = 0;
    st.seen_defs = NULL;
    st.seen_envs = NULL;
    st.rreg = rreg;
    janet_table_init(&st.seen, 0);
    marshal_one(&st, x, flags);
    /* Clean up. See comment in janet_unmarshal about autoreleasing memory on panics.*/
    janet_table_deinit(&st.seen);
    janet_v_free(st.seen_envs);
    janet_v_free(st.seen_defs);
}

typedef struct {
    jmp_buf err;
    JanetArray lookup;
    JanetTable *reg;
    JanetFuncEnv **lookup_envs;
    JanetFuncDef **lookup_defs;
    const uint8_t *start;
    const uint8_t *end;
} UnmarshalState;

#define MARSH_EOS(st, data) do { \
    if ((data) >= (st)->end) janet_panic("unexpected end of source");\
} while (0)

/* Helper to read a 32 bit integer from an unmarshal state */
static int32_t readint(UnmarshalState *st, const uint8_t **atdata) {
    const uint8_t *data = *atdata;
    int32_t ret;
    MARSH_EOS(st, data);
    if (*data < 128) {
        ret = *data++;
    } else if (*data < 192) {
        MARSH_EOS(st, data + 1);
        ret = ((data[0] & 0x3F) << 8) + data[1];
        ret = ((ret << 18) >> 18);
        data += 2;
    } else if (*data == LB_INTEGER) {
        MARSH_EOS(st, data + 4);
        ret = ((int32_t)(data[1]) << 24) |
              ((int32_t)(data[2]) << 16) |
              ((int32_t)(data[3]) << 8) |
              (int32_t)(data[4]);
        data += 5;
    } else {
        janet_panicf("expected integer, got byte %x at index %d",
                     *data,
                     data - st->start);
        ret = 0;
    }
    *atdata = data;
    return ret;
}

/* Assert a janet type */
static void janet_asserttype(Janet x, JanetType t) {
    if (!janet_checktype(x, t)) {
        janet_panicf("expected type %T, got %v", 1 << t, x);
    }
}

/* Forward declarations for mutual recursion */
static const uint8_t *unmarshal_one(
    UnmarshalState *st,
    const uint8_t *data,
    Janet *out,
    int flags);
static const uint8_t *unmarshal_one_env(
    UnmarshalState *st,
    const uint8_t *data,
    JanetFuncEnv **out,
    int flags);
static const uint8_t *unmarshal_one_def(
    UnmarshalState *st,
    const uint8_t *data,
    JanetFuncDef **out,
    int flags);
static const uint8_t *unmarshal_one_fiber(
    UnmarshalState *st,
    const uint8_t *data,
    JanetFiber **out,
    int flags);

/* Unmarshal a funcenv */
static const uint8_t *unmarshal_one_env(
    UnmarshalState *st,
    const uint8_t *data,
    JanetFuncEnv **out,
    int flags) {
    MARSH_EOS(st, data);
    if (*data == LB_FUNCENV_REF) {
        data++;
        int32_t index = readint(st, &data);
        if (index < 0 || index >= janet_v_count(st->lookup_envs))
            janet_panicf("invalid funcenv reference %d", index);
        *out = st->lookup_envs[index];
    } else {
        JanetFuncEnv *env = janet_gcalloc(JANET_MEMORY_FUNCENV, sizeof(JanetFuncEnv));
        env->length = 0;
        env->offset = 0;
        janet_v_push(st->lookup_envs, env);
        int32_t offset = readint(st, &data);
        int32_t length = readint(st, &data);
        if (offset) {
            Janet fiberv;
            /* On stack variant */
            data = unmarshal_one(st, data, &fiberv, flags);
            janet_asserttype(fiberv, JANET_FIBER);
            env->as.fiber = janet_unwrap_fiber(fiberv);
            /* Unmarshalling fiber may set values */
            if (env->offset != 0 && env->offset != offset)
                janet_panic("invalid funcenv offset");
            if (env->length != 0 && env->length != length)
                janet_panic("invalid funcenv length");
        } else {
            /* Off stack variant */
            env->as.values = malloc(sizeof(Janet) * length);
            if (!env->as.values) {
                JANET_OUT_OF_MEMORY;
            }
            for (int32_t i = 0; i < length; i++)
                data = unmarshal_one(st, data, env->as.values + i, flags);
        }
        env->offset = offset;
        env->length = length;
        *out = env;
    }
    return data;
}

/* Unmarshal a funcdef */
static const uint8_t *unmarshal_one_def(
    UnmarshalState *st,
    const uint8_t *data,
    JanetFuncDef **out,
    int flags) {
    MARSH_EOS(st, data);
    if (*data == LB_FUNCDEF_REF) {
        data++;
        int32_t index = readint(st, &data);
        if (index < 0 || index >= janet_v_count(st->lookup_defs))
            janet_panicf("invalid funcdef reference %d", index);
        *out = st->lookup_defs[index];
    } else {
        /* Initialize with values that will not break garbage collection
         * if unmarshalling fails. */
        JanetFuncDef *def = janet_gcalloc(JANET_MEMORY_FUNCDEF, sizeof(JanetFuncDef));
        def->environments_length = 0;
        def->defs_length = 0;
        def->constants_length = 0;
        def->bytecode_length = 0;
        def->name = NULL;
        def->source = NULL;
        janet_v_push(st->lookup_defs, def);

        /* Set default lengths to zero */
        int32_t bytecode_length = 0;
        int32_t constants_length = 0;
        int32_t environments_length = 0;
        int32_t defs_length = 0;

        /* Read flags and other fixed values */
        def->flags = readint(st, &data);
        def->slotcount = readint(st, &data);
        def->arity = readint(st, &data);

        /* Read some lengths */
        constants_length = readint(st, &data);
        bytecode_length = readint(st, &data);
        if (def->flags & JANET_FUNCDEF_FLAG_HASENVS)
            environments_length = readint(st, &data);
        if (def->flags & JANET_FUNCDEF_FLAG_HASDEFS)
            defs_length = readint(st, &data);

        /* Check name and source (optional) */
        if (def->flags & JANET_FUNCDEF_FLAG_HASNAME) {
            Janet x;
            data = unmarshal_one(st, data, &x, flags + 1);
            janet_asserttype(x, JANET_STRING);
            def->name = janet_unwrap_string(x);
        }
        if (def->flags & JANET_FUNCDEF_FLAG_HASSOURCE) {
            Janet x;
            data = unmarshal_one(st, data, &x, flags + 1);
            janet_asserttype(x, JANET_STRING);
            def->source = janet_unwrap_string(x);
        }

        /* Unmarshal constants */
        if (constants_length) {
            def->constants = malloc(sizeof(Janet) * constants_length);
            if (!def->constants) {
                JANET_OUT_OF_MEMORY;
            }
            for (int32_t i = 0; i < constants_length; i++)
                data = unmarshal_one(st, data, def->constants + i, flags + 1);
        } else {
            def->constants = NULL;
        }
        def->constants_length = constants_length;

        /* Unmarshal bytecode */
        def->bytecode = malloc(sizeof(uint32_t) * bytecode_length);
        if (!def->bytecode) {
            JANET_OUT_OF_MEMORY;
        }
        for (int32_t i = 0; i < bytecode_length; i++) {
            MARSH_EOS(st, data + 3);
            def->bytecode[i] =
                (uint32_t)(data[0]) |
                ((uint32_t)(data[1]) << 8) |
                ((uint32_t)(data[2]) << 16) |
                ((uint32_t)(data[3]) << 24);
            data += 4;
        }
        def->bytecode_length = bytecode_length;

        /* Unmarshal environments */
        if (def->flags & JANET_FUNCDEF_FLAG_HASENVS) {
            def->environments = calloc(1, sizeof(int32_t) * environments_length);
            if (!def->environments) {
                JANET_OUT_OF_MEMORY;
            }
            for (int32_t i = 0; i < environments_length; i++) {
                def->environments[i] = readint(st, &data);
            }
        } else {
            def->environments = NULL;
        }
        def->environments_length = environments_length;

        /* Unmarshal sub funcdefs */
        if (def->flags & JANET_FUNCDEF_FLAG_HASDEFS) {
            def->defs = calloc(1, sizeof(JanetFuncDef *) * defs_length);
            if (!def->defs) {
                JANET_OUT_OF_MEMORY;
            }
            for (int32_t i = 0; i < defs_length; i++) {
                data = unmarshal_one_def(st, data, def->defs + i, flags + 1);
            }
        } else {
            def->defs = NULL;
        }
        def->defs_length = defs_length;

        /* Unmarshal source maps if needed */
        if (def->flags & JANET_FUNCDEF_FLAG_HASSOURCEMAP) {
            int32_t current = 0;
            def->sourcemap = malloc(sizeof(JanetSourceMapping) * bytecode_length);
            if (!def->sourcemap) {
                JANET_OUT_OF_MEMORY;
            }
            for (int32_t i = 0; i < bytecode_length; i++) {
                current += readint(st, &data);
                def->sourcemap[i].start = current;
                current += readint(st, &data);
                def->sourcemap[i].end = current;
            }
        } else {
            def->sourcemap = NULL;
        }

        /* Validate */
        if (janet_verify(def))
            janet_panic("funcdef has invalid bytecode");

        /* Set def */
        *out = def;
    }
    return data;
}

/* Unmarshal a fiber */
static const uint8_t *unmarshal_one_fiber(
    UnmarshalState *st,
    const uint8_t *data,
    JanetFiber **out,
    int flags) {

    /* Initialize a new fiber */
    JanetFiber *fiber = janet_gcalloc(JANET_MEMORY_FIBER, sizeof(JanetFiber));
    fiber->flags = 0;
    fiber->frame = 0;
    fiber->stackstart = 0;
    fiber->stacktop = 0;
    fiber->capacity = 0;
    fiber->maxstack = 0;
    fiber->data = NULL;
    fiber->child = NULL;

    /* Push fiber to seen stack */
    janet_array_push(&st->lookup, janet_wrap_fiber(fiber));

    /* Set frame later so fiber can be GCed at anytime if unmarshalling fails */
    int32_t frame = 0;
    int32_t stack = 0;
    int32_t stacktop = 0;

    /* Read ints */
    fiber->flags = readint(st, &data);
    frame = readint(st, &data);
    fiber->stackstart = readint(st, &data);
    fiber->stacktop = readint(st, &data);
    fiber->maxstack = readint(st, &data);

    /* Check for bad flags and ints */
    if ((int32_t)(frame + JANET_FRAME_SIZE) > fiber->stackstart ||
            fiber->stackstart > fiber->stacktop ||
            fiber->stacktop > fiber->maxstack) {
        janet_panic("fiber has incorrect stack setup");
    }

    /* Allocate stack memory */
    fiber->capacity = fiber->stacktop + 10;
    fiber->data = malloc(sizeof(Janet) * fiber->capacity);
    if (!fiber->data) {
        JANET_OUT_OF_MEMORY;
    }

    /* get frames */
    stack = frame;
    stacktop = fiber->stackstart - JANET_FRAME_SIZE;
    while (stack > 0) {
        JanetFunction *func = NULL;
        JanetFuncDef *def = NULL;
        JanetFuncEnv *env = NULL;
        int32_t frameflags = readint(st, &data);
        int32_t prevframe = readint(st, &data);
        int32_t pcdiff = readint(st, &data);

        /* Get frame items */
        Janet *framestack = fiber->data + stack;
        JanetStackFrame *framep = janet_stack_frame(framestack);

        /* Get function */
        Janet funcv;
        data = unmarshal_one(st, data, &funcv, flags + 1);
        janet_asserttype(funcv, JANET_FUNCTION);
        func = janet_unwrap_function(funcv);
        def = func->def;

        /* Check env */
        if (frameflags & JANET_STACKFRAME_HASENV) {
            frameflags &= ~JANET_STACKFRAME_HASENV;
            int32_t offset = stack;
            int32_t length = stacktop - stack;
            data = unmarshal_one_env(st, data, &env, flags + 1);
            if (env->offset != 0 && env->offset != offset)
                janet_panic("funcenv offset does not match fiber frame");
            if (env->length != 0 && env->length != length)
                janet_panic("funcenv length does not match fiber frame");
            env->offset = offset;
            env->length = length;
        }

        /* Error checking */
        int32_t expected_framesize = def->slotcount;
        if (expected_framesize != stacktop - stack) {
            janet_panic("fiber stackframe size mismatch");
        }
        if (pcdiff < 0 || pcdiff >= def->bytecode_length) {
            janet_panic("fiber stackframe has invalid pc");
        }
        if ((int32_t)(prevframe + JANET_FRAME_SIZE) > stack) {
            janet_panic("fibre stackframe does not align with previous frame");
        }

        /* Get stack items */
        for (int32_t i = stack; i < stacktop; i++)
            data = unmarshal_one(st, data, fiber->data + i, flags + 1);

        /* Set frame */
        framep->env = env;
        framep->pc = def->bytecode + pcdiff;
        framep->prevframe = prevframe;
        framep->flags = frameflags;
        framep->func = func;

        /* Goto previous frame */
        stacktop = stack - JANET_FRAME_SIZE;
        stack = prevframe;
    }
    if (stack < 0) {
        janet_panic("fiber has too many stackframes");
    }

    /* Check for child fiber */
    if (fiber->flags & JANET_FIBER_FLAG_HASCHILD) {
        Janet fiberv;
        fiber->flags &= ~JANET_FIBER_FLAG_HASCHILD;
        data = unmarshal_one(st, data, &fiberv, flags + 1);
        janet_asserttype(fiberv, JANET_FIBER);
        fiber->child = janet_unwrap_fiber(fiberv);
    }

    /* Return data */
    fiber->frame = frame;
    *out = fiber;
    return data;
}

void janet_unmarshal_int(JanetMarshalContext *ctx, int32_t *i) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    *i = readint(st, &(ctx->data));
};

void janet_unmarshal_uint(JanetMarshalContext *ctx, uint32_t *i) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    *i = (uint32_t)readint(st, &(ctx->data));
};

void janet_unmarshal_size(JanetMarshalContext *ctx, size_t *i) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    *i = (size_t)readint(st, &(ctx->data));
};

void janet_unmarshal_byte(JanetMarshalContext *ctx, uint8_t *b) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    MARSH_EOS(st, ctx->data);
    *b = *(ctx->data++);
};

void janet_unmarshal_bytes(JanetMarshalContext *ctx, uint8_t *dest, int32_t len) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    MARSH_EOS(st, ctx->data + len - 1);
    memcpy(dest, ctx->data, len);
    ctx->data += len;
}

void janet_unmarshal_janet(JanetMarshalContext *ctx, Janet *out) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    ctx->data = unmarshal_one(st, ctx->data, out, ctx->flags);
}

static const uint8_t *unmarshal_one_abstract(UnmarshalState *st, const uint8_t *data, Janet *out, int flags) {
    Janet key;
    data = unmarshal_one(st, data, &key, flags + 1);
    const JanetAbstractType *at = janet_get_abstract_type(key);
    if (at == NULL) return NULL;
    if (at->unmarshal) {
        void *p = janet_abstract(at, readint(st, &data));
        JanetMarshalContext context = {NULL, st, flags, data};
        at->unmarshal(p, &context);
        *out = janet_wrap_abstract(p);
        return data;
    }
    return NULL;
}

static const uint8_t *unmarshal_one(
    UnmarshalState *st,
    const uint8_t *data,
    Janet *out,
    int flags) {
    uint8_t lead;
    MARSH_STACKCHECK;
    MARSH_EOS(st, data);
    lead = data[0];
    if (lead < 200) {
        *out = janet_wrap_integer(readint(st, &data));
        return data;
    }
    switch (lead) {
        case LB_NIL:
            *out = janet_wrap_nil();
            return data + 1;
        case LB_FALSE:
            *out = janet_wrap_false();
            return data + 1;
        case LB_TRUE:
            *out = janet_wrap_true();
            return data + 1;
        case LB_INTEGER:
            /* Long integer */
            MARSH_EOS(st, data + 4);
            *out = janet_wrap_integer(
                       (data[4]) |
                       (data[3] << 8) |
                       (data[2] << 16) |
                       (data[1] << 24));
            return data + 5;
        case LB_REAL:
            /* Real */
        {
            union {
                double d;
                uint8_t bytes[8];
            } u;
            MARSH_EOS(st, data + 8);
#ifdef JANET_BIG_ENDIAN
            u.bytes[0] = data[8];
            u.bytes[1] = data[7];
            u.bytes[2] = data[6];
            u.bytes[5] = data[5];
            u.bytes[4] = data[4];
            u.bytes[5] = data[3];
            u.bytes[6] = data[2];
            u.bytes[7] = data[1];
#else
            memcpy(&u.bytes, data + 1, sizeof(double));
#endif
            *out = janet_wrap_number(u.d);
            janet_array_push(&st->lookup, *out);
            return data + 9;
        }
        case LB_STRING:
        case LB_SYMBOL:
        case LB_BUFFER:
        case LB_KEYWORD:
        case LB_REGISTRY: {
            data++;
            int32_t len = readint(st, &data);
            MARSH_EOS(st, data - 1 + len);
            if (lead == LB_STRING) {
                const uint8_t *str = janet_string(data, len);
                *out = janet_wrap_string(str);
            } else if (lead == LB_SYMBOL) {
                const uint8_t *str = janet_symbol(data, len);
                *out = janet_wrap_symbol(str);
            } else if (lead == LB_KEYWORD) {
                const uint8_t *str = janet_keyword(data, len);
                *out = janet_wrap_keyword(str);
            } else if (lead == LB_REGISTRY) {
                if (st->reg) {
                    Janet regkey = janet_symbolv(data, len);
                    *out = janet_table_get(st->reg, regkey);
                } else {
                    *out = janet_wrap_nil();
                }
            } else { /* (lead == LB_BUFFER) */
                JanetBuffer *buffer = janet_buffer(len);
                buffer->count = len;
                memcpy(buffer->data, data, len);
                *out = janet_wrap_buffer(buffer);
            }
            janet_array_push(&st->lookup, *out);
            return data + len;
        }
        case LB_FIBER: {
            JanetFiber *fiber;
            data = unmarshal_one_fiber(st, data + 1, &fiber, flags);
            *out = janet_wrap_fiber(fiber);
            return data;
        }
        case LB_FUNCTION: {
            JanetFunction *func;
            JanetFuncDef *def;
            data = unmarshal_one_def(st, data + 1, &def, flags + 1);
            func = janet_gcalloc(JANET_MEMORY_FUNCTION, sizeof(JanetFunction) +
                                 def->environments_length * sizeof(JanetFuncEnv));
            func->def = def;
            *out = janet_wrap_function(func);
            janet_array_push(&st->lookup, *out);
            for (int32_t i = 0; i < def->environments_length; i++) {
                data = unmarshal_one_env(st, data, &(func->envs[i]), flags + 1);
            }
            return data;
        }
        case LB_ABSTRACT: {
            data++;
            return unmarshal_one_abstract(st, data, out, flags);
        }
        case LB_REFERENCE:
        case LB_ARRAY:
        case LB_TUPLE:
        case LB_STRUCT:
        case LB_TABLE:
        case LB_TABLE_PROTO:
            /* Things that open with integers */
        {
            data++;
            int32_t len = readint(st, &data);
            if (lead == LB_ARRAY) {
                /* Array */
                JanetArray *array = janet_array(len);
                array->count = len;
                *out = janet_wrap_array(array);
                janet_array_push(&st->lookup, *out);
                for (int32_t i = 0; i < len; i++) {
                    data = unmarshal_one(st, data, array->data + i, flags + 1);
                }
            } else if (lead == LB_TUPLE) {
                /* Tuple */
                Janet *tup = janet_tuple_begin(len);
                int32_t flag = readint(st, &data);
                janet_tuple_flag(tup) |= flag << 16;
                for (int32_t i = 0; i < len; i++) {
                    data = unmarshal_one(st, data, tup + i, flags + 1);
                }
                *out = janet_wrap_tuple(janet_tuple_end(tup));
                janet_array_push(&st->lookup, *out);
            } else if (lead == LB_STRUCT) {
                /* Struct */
                JanetKV *struct_ = janet_struct_begin(len);
                for (int32_t i = 0; i < len; i++) {
                    Janet key, value;
                    data = unmarshal_one(st, data, &key, flags + 1);
                    data = unmarshal_one(st, data, &value, flags + 1);
                    janet_struct_put(struct_, key, value);
                }
                *out = janet_wrap_struct(janet_struct_end(struct_));
                janet_array_push(&st->lookup, *out);
            } else if (lead == LB_REFERENCE) {
                if (len < 0 || len >= st->lookup.count)
                    janet_panicf("invalid reference %d", len);
                *out = st->lookup.data[len];
            } else {
                /* Table */
                JanetTable *t = janet_table(len);
                *out = janet_wrap_table(t);
                janet_array_push(&st->lookup, *out);
                if (lead == LB_TABLE_PROTO) {
                    Janet proto;
                    data = unmarshal_one(st, data, &proto, flags + 1);
                    janet_asserttype(proto, JANET_TABLE);
                    t->proto = janet_unwrap_table(proto);
                }
                for (int32_t i = 0; i < len; i++) {
                    Janet key, value;
                    data = unmarshal_one(st, data, &key, flags + 1);
                    data = unmarshal_one(st, data, &value, flags + 1);
                    janet_table_put(t, key, value);
                }
            }
            return data;
        }
        default: {
            janet_panicf("unknown byte %x at index %d",
                         *data,
                         (int)(data - st->start));
            return NULL;
        }
    }
#undef EXTRA
}

Janet janet_unmarshal(
    const uint8_t *bytes,
    size_t len,
    int flags,
    JanetTable *reg,
    const uint8_t **next) {
    UnmarshalState st;
    st.start = bytes;
    st.end = bytes + len;
    st.lookup_defs = NULL;
    st.lookup_envs = NULL;
    st.reg = reg;
    janet_array_init(&st.lookup, 0);
    Janet out;
    const uint8_t *nextbytes = unmarshal_one(&st, bytes, &out, flags);
    if (next) *next = nextbytes;
    /* Clean up - this should be auto released on panics, TODO. We should
     * change the vector implementation to track allocations for auto release, and
     * make st.lookup auto release as well, or move to heap. */
    janet_array_deinit(&st.lookup);
    janet_v_free(st.lookup_defs);
    janet_v_free(st.lookup_envs);
    return out;
}

/* C functions */

static Janet cfun_env_lookup(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    JanetTable *env = janet_gettable(argv, 0);
    return janet_wrap_table(janet_env_lookup(env));
}

static Janet cfun_marshal(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, 2);
    JanetBuffer *buffer;
    JanetTable *rreg = NULL;
    if (argc > 1) {
        rreg = janet_gettable(argv, 1);
    }
    if (argc > 2) {
        buffer = janet_getbuffer(argv, 2);
    } else {
        buffer = janet_buffer(10);
    }
    janet_marshal(buffer, argv[0], rreg, 0);
    return janet_wrap_buffer(buffer);
}

static Janet cfun_unmarshal(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, 2);
    JanetByteView view = janet_getbytes(argv, 0);
    JanetTable *reg = NULL;
    if (argc > 1) {
        reg = janet_gettable(argv, 1);
    }
    return janet_unmarshal(view.bytes, (size_t) view.len, 0, reg, NULL);
}

static const JanetReg marsh_cfuns[] = {
    {
        "marshal", cfun_marshal,
        JDOC("(marshal x [,reverse-lookup [,buffer]])\n\n"
             "Marshal a janet value into a buffer and return the buffer. The buffer "
             "can the later be unmarshalled to reconstruct the initial value. "
             "Optionally, one can pass in a reverse lookup table to not marshal "
             "aliased values that are found in the table. Then a forward"
             "lookup table can be used to recover the original janet value when "
             "unmarshalling.")
    },
    {
        "unmarshal", cfun_unmarshal,
        JDOC("(unmarshal buffer [,lookup])\n\n"
             "Unmarshal a janet value from a buffer. An optional lookup table "
             "can be provided to allow for aliases to be resolved. Returns the value "
             "unmarshalled from the buffer.")
    },
    {
        "env-lookup", cfun_env_lookup,
        JDOC("(env-lookup env)\n\n"
             "Creates a forward lookup table for unmarshalling from an environment. "
             "To create a reverse lookup table, use the invert function to swap keys "
             "and values in the returned table.")
    },
    {NULL, NULL, NULL}
};

/* Module entry point */
void janet_lib_marsh(JanetTable *env) {
    janet_core_cfuns(env, NULL, marsh_cfuns);
}

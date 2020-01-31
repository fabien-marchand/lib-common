/***************************************************************************/
/*                                                                         */
/* Copyright 2019 INTERSEC SA                                              */
/*                                                                         */
/* Licensed under the Apache License, Version 2.0 (the "License");         */
/* you may not use this file except in compliance with the License.        */
/* You may obtain a copy of the License at                                 */
/*                                                                         */
/*     http://www.apache.org/licenses/LICENSE-2.0                          */
/*                                                                         */
/* Unless required by applicable law or agreed to in writing, software     */
/* distributed under the License is distributed on an "AS IS" BASIS,       */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*/
/* See the License for the specific language governing permissions and     */
/* limitations under the License.                                          */
/*                                                                         */
/***************************************************************************/

#include <math.h>

#include <lib-common/yaml.h>
#include <lib-common/parsing-helpers.h>
#include <lib-common/log.h>
#include <lib-common/file.h>
#include <lib-common/unix.h>
#include <lib-common/iop.h>
#include <lib-common/iop-json.h>

static struct yaml_g {
    logger_t logger;
} yaml_g = {
#define _G yaml_g
    .logger = LOGGER_INIT(NULL, "yaml", LOG_INHERITS),
};

/* Missing features:
 *
 * #1
 * Tab characters are forbidden, because it makes the indentation computation
 * harder than with simple spaces. It could be handled properly however.
 */

/* {{{ Parsing types definitions */
/* {{{ Presentation */

qm_kvec_t(yaml_pres_node, lstr_t, const yaml__presentation_node__t * nonnull,
          qhash_lstr_hash, qhash_lstr_equal);

/* This is a yaml.DocumentPresentation transformed into a hashmap. */
typedef struct yaml_presentation_t {
    qm_t(yaml_pres_node) nodes;
} yaml_presentation_t;

/* Presentation details currently being constructed */
typedef struct yaml_env_presentation_t {
    /* Presentation node of the last parsed element.
     *
     * This can point to:
     *  * the node of the last parsed yaml_data_t object.
     *  * the node of a sequence element.
     *  * the node of an object key.
     *
     * It can be NULL at the very beginning of the document.
     */
    yaml__presentation_node__t * nonnull * nullable last_node;

    /* Presentation detail for the next element to generate.
     *
     * When parsing presentation data that applies to the next element (for
     * example, with prefix comments), this element is filled, and retrieved
     * when the next element is created.
     */
    yaml__presentation_node__t * nullable next_node;
} yaml_env_presentation_t;

/* }}} */
/* {{{ Variables */

typedef struct yaml_variable_t {
    /* Data using the variable. The set value for the variable will be set
     * in the data, or replace it. */
    yaml_data_t *data;

    /* Is the variable in a string, or raw?
     *
     * Raw means any AST is valid:
     *
     * foo: $bar
     *
     * In string means it must be a string value, and will be set in the
     * data:
     *
     * addr: "$host:ip"
     */
    bool in_string;
} yaml_variable_t;
qvector_t(yaml_variable, yaml_variable_t);

qm_kvec_t(yaml_vars, lstr_t, qv_t(yaml_variable), qhash_lstr_hash,
          qhash_lstr_equal);

/* }}} */

qvector_t(yaml_parse, yaml_parse_t *);

typedef struct yaml_included_file_t {
    /* Parsing context that included the current file. */
    const yaml_parse_t * nonnull parent;

    /** Data from the including file, that caused the inclusion.
     *
     * This is the "!include <file>" data. It is not stored in the including
     * yaml_parse_t object, as the inclusion is transparent in its AST.
     * However, it is stored here to provide proper error messages.
     */
    yaml_data_t data;
} yaml_included_file_t;

typedef struct yaml_parse_t {
    /* String to parse. */
    pstream_t ps;

    /* Name of the file being parsed.
     *
     * This is the name of the file as it was given to yaml_parse_attach_file.
     * It can be an absolute or a relative path.
     *
     * NULL if a stream is being parsed.
     */
    const char * nullable filepath;

    /* Fullpath to the file being parsed.
     *
     * LSTR_NULL_V if a stream is being parsed.
     */
    lstr_t fullpath;

    /* mmap'ed contents of the file. */
    lstr_t file_contents;

    /* Bitfield of yaml_parse_flags_t elements. */
    int flags;

    /* Current line number. */
    uint32_t line_number;

    /* Pointer to the first character of the current line.
     * Used to compute current column number of ps->s */
    const char *pos_newline;

    /* Error buffer. */
    sb_t err;

    /* Presentation details.
     *
     * Can be NULL if the user did not asked for presentation details.
     */
    yaml_env_presentation_t * nullable pres;

    /* Included files.
     *
     * List of parse context of every subfiles included. */
    qv_t(yaml_parse) subfiles;

    /* Included details.
     *
     * This is set if the current file was included from another file.
     */
    yaml_included_file_t * nullable included;

    qm_t(yaml_vars) variables;
} yaml_parse_t;

qvector_t(override_nodes, yaml__presentation_override_node__t);

/* Presentation details of an override.
 *
 * This object is used to build a yaml.PresentationOverride. See the document
 * of this object for more details.
 */
typedef struct yaml_presentation_override_t {
    /* List of nodes of the override. */
    qv_t(override_nodes) nodes;

    /* Current path from the override root point.
     *
     * Describe the path not from the document's root, but from the override's
     * root. Used to build the nodes.
     */
    sb_t path;
} yaml_presentation_override_t;

/* Node to override, when packing. */
typedef struct yaml_pack_override_node_t {
    /* Data related to the override.
     *
     * When beginning to pack an override, this is set to the original data,
     * the one replaced by the override on parsing.
     * When the AST is packed, this data is retrieved and the overridden data
     * is swapped and stored here.
     * Then, the override data is packed using those datas.
     */
    const yaml_data_t * nullable data;

    /* If the data has been found and retrieved. If false, the data has either
     * not yet been found while packing the AST, or the node has disappeared
     * from the AST.
     */
    bool found;
} yaml_pack_override_node_t;

qm_kvec_t(override_nodes, lstr_t, yaml_pack_override_node_t,
          qhash_lstr_hash, qhash_lstr_equal);

/* Description of an override, used when packing.
 *
 * This object is used to properly pack overrides. It is the equivalent of
 * a yaml.PresentationOverride, but with a qm instead of an array, so that
 * nodes that were overridden can easily find the original value to repack.
 */
typedef struct yaml_pack_override_t {
    /* Mappings of absolute paths to override pack nodes.
     *
     * The paths are not the same as the ones in the presentation IOP. They
     * are absolute path, from the root document and not from the override's
     * root.
     *
     * This is needed in order to handle overrides of included data. The
     * path in the included data is relative from the root of its file, which
     * may be different from the path of the override nodes (for example,
     * if the override was done in a file that included a file including the
     * current file.
     * */
    qm_t(override_nodes) nodes;

    /** List of the absolute paths.
     *
     * Used to iterate on the nodes in the right order when repacking the
     * override objet.
     */
    qv_t(lstr) ordered_paths;

    /* Original override presentation object.
     */
    const yaml__presentation_override__t * nonnull presentation;
} yaml_pack_override_t;
qvector_t(pack_override, yaml_pack_override_t);

/* }}} */
/* {{{ IOP helpers */
/* {{{ IOP scalar */

static void
t_yaml_data_to_iop(const yaml_data_t * nonnull data,
                   yaml__data__t * nonnull out)
{
    yaml__scalar_value__t *scalar;

    out->tag = data->tag;

    /* TODO: for the moment, only scalars can be overridden, so only scalars
     * needs to be serialized. Once overrides can replace any data, this
     * function will have to be modified */
    assert (data->type == YAML_DATA_SCALAR);
    scalar = IOP_UNION_SET(yaml__data_value, &out->value, scalar);

    switch (data->scalar.type) {
#define CASE(_name, _prefix)                                                 \
      case YAML_SCALAR_##_name:                                              \
        *IOP_UNION_SET(yaml__scalar_value, scalar, _prefix)                  \
            = data->scalar._prefix;                                          \
        break

      CASE(STRING, s);
      CASE(DOUBLE, d);
      CASE(UINT, u);
      CASE(INT, i);
      CASE(BOOL, b);

#undef CASE

      case YAML_SCALAR_NULL:
        IOP_UNION_SET_V(yaml__scalar_value, scalar, nil);
        break;
    }
}

static void
t_iop_data_to_yaml(const yaml__data__t * nonnull data,
                   yaml_data_t * nonnull out)
{
    assert (IOP_UNION_IS(yaml__data_value, &data->value, scalar));

    IOP_UNION_SWITCH(&data->value.scalar) {
      IOP_UNION_CASE(yaml__scalar_value, &data->value.scalar, s, s) {
        yaml_data_set_string(out, s);
      }
      IOP_UNION_CASE(yaml__scalar_value, &data->value.scalar, d, d) {
        yaml_data_set_double(out, d);
      }
      IOP_UNION_CASE(yaml__scalar_value, &data->value.scalar, u, u) {
        yaml_data_set_uint(out, u);
      }
      IOP_UNION_CASE(yaml__scalar_value, &data->value.scalar, i, i) {
        yaml_data_set_int(out, i);
      }
      IOP_UNION_CASE(yaml__scalar_value, &data->value.scalar, b, b)  {
        yaml_data_set_bool(out, b);
      }
      IOP_UNION_CASE_V(yaml__scalar_value, &data->value.scalar, nil)  {
        yaml_data_set_null(out);
      }
    }
    out->tag = data->tag;
}

/* }}} */
/* {{{ IOP Presentation Override */

static yaml__presentation_override__t * nonnull
t_presentation_override_to_iop(
    const yaml_presentation_override_t * nonnull pres,
    const yaml_data_t * nonnull override_data
)
{
    yaml__presentation_override__t *out;

    out = t_iop_new(yaml__presentation_override);
    out->nodes = IOP_TYPED_ARRAY_TAB(yaml__presentation_override_node,
                                     &pres->nodes);
    t_yaml_data_get_presentation(override_data, &out->presentation);

    return out;
}

/* }}} */
/* }}} */
/* {{{ Utils */

static const char * nonnull
yaml_scalar_get_type(const yaml_scalar_t * nonnull scalar, bool has_tag)
{
    switch (scalar->type) {
      case YAML_SCALAR_STRING:
        return has_tag ? "a tagged string value" : "a string value";
      case YAML_SCALAR_DOUBLE:
        return has_tag ? "a tagged double value" : "a double value";
      case YAML_SCALAR_UINT:
        return has_tag ? "a tagged unsigned integer value"
                       : "an unsigned integer value";
      case YAML_SCALAR_INT:
        return has_tag ? "a tagged integer value" : "an integer value";
      case YAML_SCALAR_BOOL:
        return has_tag ? "a tagged boolean value" : "a boolean value";
      case YAML_SCALAR_NULL:
        return has_tag ? "a tagged null value" : "a null value";
    }

    assert (false);
    return "";
}

const char *yaml_data_get_type(const yaml_data_t *data, bool ignore_tag)
{
    bool has_tag = data->tag.s && !ignore_tag;

    switch (data->type) {
      case YAML_DATA_OBJ:
        return has_tag ? "a tagged object" : "an object";
      case YAML_DATA_SEQ:
        return has_tag ? "a tagged sequence" : "a sequence";
      case YAML_DATA_SCALAR:
        return yaml_scalar_get_type(&data->scalar, has_tag);
    }

    assert (false);
    return "";
}

static const char *yaml_data_get_data_type(const yaml_data_t *data)
{
    switch (data->type) {
      case YAML_DATA_OBJ:
        return "an object";
      case YAML_DATA_SEQ:
        return "a sequence";
      case YAML_DATA_SCALAR:
        return "a scalar";
    }

    assert (false);
    return "";
}

lstr_t yaml_span_to_lstr(const yaml_span_t * nonnull span)
{
    return LSTR_PTR_V(span->start.s, span->end.s);
}

static uint32_t yaml_env_get_column_nb(const yaml_parse_t *env)
{
    return env->ps.s - env->pos_newline + 1;
}

static yaml_pos_t yaml_env_get_pos(const yaml_parse_t *env)
{
    return (yaml_pos_t){
        .line_nb = env->line_number,
        .col_nb = yaml_env_get_column_nb(env),
        .s = env->ps.s,
    };
}

static inline void yaml_env_skipc(yaml_parse_t *env)
{
    IGNORE(ps_getc(&env->ps));
}

static void yaml_span_init(yaml_span_t * nonnull span,
                           const yaml_parse_t * nonnull env,
                           yaml_pos_t pos_start, yaml_pos_t pos_end)
{
    p_clear(span, 1);
    span->start = pos_start;
    span->end = pos_end;
    span->env = env;
}

static void
yaml_env_start_data_with_pos(yaml_parse_t * nonnull env,
                             yaml_data_type_t type, yaml_pos_t pos_start,
                             yaml_data_t * nonnull out)
{
    p_clear(out, 1);
    out->type = type;
    yaml_span_init(&out->span, env, pos_start, pos_start);

    if (env->pres && env->pres->next_node) {
        /* Get the saved presentation details that were stored for the next
         * data (ie this one).
         */
        out->presentation = env->pres->next_node;
        env->pres->next_node = NULL;

        logger_trace(&_G.logger, 2, "adding prefixed presentation details "
                     "for data starting at "YAML_POS_FMT,
                     YAML_POS_ARG(pos_start));
    }
}

static void
yaml_env_start_data(yaml_parse_t * nonnull env, yaml_data_type_t type,
                    yaml_data_t * nonnull out)
{
    yaml_env_start_data_with_pos(env, type, yaml_env_get_pos(env), out);
}

static void
yaml_env_end_data_with_pos(yaml_parse_t * nonnull env, yaml_pos_t pos_end,
                           yaml_data_t * nonnull out)
{
    out->span.end = pos_end;

    if (env->pres) {
        env->pres->last_node = &out->presentation;
    }
}

static void
yaml_env_end_data(yaml_parse_t * nonnull env, yaml_data_t * nonnull out)
{
    yaml_env_end_data_with_pos(env, yaml_env_get_pos(env), out);
}

/* }}} */
/* {{{ Errors */

typedef enum yaml_error_t {
    YAML_ERR_BAD_KEY,
    YAML_ERR_BAD_STRING,
    YAML_ERR_MISSING_DATA,
    YAML_ERR_WRONG_DATA,
    YAML_ERR_WRONG_INDENT,
    YAML_ERR_WRONG_OBJECT,
    YAML_ERR_TAB_CHARACTER,
    YAML_ERR_INVALID_TAG,
    YAML_ERR_EXTRA_DATA,
    YAML_ERR_INVALID_INCLUDE,
    YAML_ERR_INVALID_OVERRIDE,
} yaml_error_t;

static int yaml_env_set_err_at(yaml_parse_t * nonnull env,
                               const yaml_span_t * nonnull span,
                               yaml_error_t type, const char * nonnull msg)
{
    SB_1k(err);

    switch (type) {
      case YAML_ERR_BAD_KEY:
        sb_addf(&err, "invalid key, %s", msg);
        break;
      case YAML_ERR_BAD_STRING:
        sb_addf(&err, "expected string, %s", msg);
        break;
      case YAML_ERR_MISSING_DATA:
        sb_addf(&err, "missing data, %s", msg);
        break;
      case YAML_ERR_WRONG_DATA:
        sb_addf(&err, "wrong type of data, %s", msg);
        break;
      case YAML_ERR_WRONG_INDENT:
        sb_addf(&err, "wrong indentation, %s", msg);
        break;
      case YAML_ERR_WRONG_OBJECT:
        sb_addf(&err, "wrong object, %s", msg);
        break;
      case YAML_ERR_TAB_CHARACTER:
        sb_addf(&err, "tab character detected, %s", msg);
        break;
      case YAML_ERR_INVALID_TAG:
        sb_addf(&err, "invalid tag, %s", msg);
        break;
      case YAML_ERR_EXTRA_DATA:
        sb_addf(&err, "extra characters after data, %s", msg);
        break;
      case YAML_ERR_INVALID_INCLUDE:
        sb_addf(&err, "invalid include, %s", msg);
        break;
      case YAML_ERR_INVALID_OVERRIDE:
        sb_addf(&err, "cannot change types of data in override, %s", msg);
        break;
    }

    yaml_parse_pretty_print_err(span, LSTR_SB_V(&err), &env->err);

    return -1;
}

static int yaml_env_set_err(yaml_parse_t * nonnull env, yaml_error_t type,
                            const char * nonnull msg)
{
    yaml_span_t span;
    yaml_pos_t start;
    yaml_pos_t end;


    /* build a span on the current position, to have a cursor on this
     * character in the pretty printed error message. */
    start = yaml_env_get_pos(env);
    end = start;
    end.col_nb++;
    end.s++;
    yaml_span_init(&span, env, start, end);

    return yaml_env_set_err_at(env, &span, type, msg);
}

/* }}} */
/* {{{ Parser */

static int t_yaml_env_parse_data(yaml_parse_t *env, const uint32_t min_indent,
                                 yaml_data_t *out);

/* {{{ Presentation utils */

static yaml__presentation_node__t * nonnull
t_yaml_env_pres_get_current_node(yaml_env_presentation_t * nonnull pres)
{
    /* last_node should be set, otherwise this means we are at the very
     * beginning of the document, and we should parse presentation data
     * as prefix rather than inline. */
    assert (pres->last_node);
    if (!(*pres->last_node)) {
        *pres->last_node = t_iop_new(yaml__presentation_node);
    }
    return *pres->last_node;
}

static yaml__presentation_node__t * nonnull
t_yaml_env_pres_get_next_node(yaml_env_presentation_t * nonnull pres)
{
    if (!pres->next_node) {
        pres->next_node = t_iop_new(yaml__presentation_node);
    }

    return pres->next_node;
}

static void t_yaml_env_handle_comment_ps(yaml_parse_t * nonnull env,
                                         pstream_t comment_ps, bool prefix,
                                         qv_t(lstr) * nonnull prefix_comments)
{
    lstr_t comment;

    if (!env->pres) {
        return;
    }

    comment_ps.s_end = env->ps.s;
    ps_skipc(&comment_ps, '#');
    comment = lstr_trim(LSTR_PS_V(&comment_ps));

    if (prefix) {
        if (prefix_comments->len == 0) {
            t_qv_init(prefix_comments, 1);
        }
        qv_append(prefix_comments, comment);
        logger_trace(&_G.logger, 2, "adding prefix comment `%pL`", &comment);
    } else {
        yaml__presentation_node__t *pnode;

        pnode = t_yaml_env_pres_get_current_node(env->pres);
        assert (pnode->inline_comment.len == 0);
        pnode->inline_comment = comment;
        if (env->pres->last_node) {
            logger_trace(&_G.logger, 2, "adding inline comment `%pL`",
                         &comment);
        }
    }
}

static void
yaml_env_set_prefix_comments(yaml_parse_t * nonnull env,
                             qv_t(lstr) * nonnull prefix_comments)
{
    yaml__presentation_node__t *pnode;

    if (!env->pres || prefix_comments->len == 0) {
        return;
    }

    pnode = t_yaml_env_pres_get_next_node(env->pres);
    pnode->prefix_comments = IOP_TYPED_ARRAY_TAB(lstr, prefix_comments);
}

static void t_yaml_env_pres_set_flow_mode(yaml_parse_t * nonnull env)
{
    yaml__presentation_node__t *pnode;

    if (!env->pres) {
        return;
    }

    pnode = t_yaml_env_pres_get_current_node(env->pres);
    pnode->flow_mode = true;
    logger_trace(&_G.logger, 2, "set flow mode");
}

static void t_yaml_env_pres_add_empty_line(yaml_parse_t * nonnull env)
{
    yaml__presentation_node__t *pnode;

    if (!env->pres) {
        return;
    }

    pnode = t_yaml_env_pres_get_next_node(env->pres);
    pnode->empty_lines = MIN(pnode->empty_lines + 1, 2);
}

/* }}} */
/* {{{ Utils */

static void log_new_data(const yaml_data_t * nonnull data)
{
    if (logger_is_traced(&_G.logger, 2)) {
        logger_trace_scope(&_G.logger, 2);
        logger_cont("parsed %s from "YAML_POS_FMT" up to "YAML_POS_FMT,
                    yaml_data_get_type(data, false),
                    YAML_POS_ARG(data->span.start),
                    YAML_POS_ARG(data->span.end));
        if (data->type == YAML_DATA_SCALAR) {
            lstr_t span = yaml_span_to_lstr(&data->span);

            logger_cont(": %pL", &span);
        }
    }
}

static int yaml_env_ltrim(yaml_parse_t *env)
{
    pstream_t comment_ps = ps_init(NULL, 0);
    bool in_comment = false;
    bool in_new_line = yaml_env_get_column_nb(env) == 1;
    qv_t(lstr) prefix_comments;

    p_clear(&prefix_comments, 1);

    while (!ps_done(&env->ps)) {
        int c = ps_peekc(env->ps);

        if (c == '#') {
            if (!in_comment) {
                in_comment = true;
                comment_ps = env->ps;
            }
        } else
        if (c == '\n') {
            if (env->pos_newline == env->ps.s) {
                /* Two \n in a row, indicating an empty line. Save this
                 * is the presentation data. */
                t_yaml_env_pres_add_empty_line(env);
            }
            env->line_number++;
            env->pos_newline = env->ps.s + 1;
            in_comment = false;
            if (comment_ps.s != NULL) {
                t_yaml_env_handle_comment_ps(env, comment_ps, in_new_line,
                                             &prefix_comments);
                comment_ps.s = NULL;
            }
            in_new_line = true;
        } else
        if (c == '\t') {
            return yaml_env_set_err(env, YAML_ERR_TAB_CHARACTER,
                                    "cannot use tab characters for "
                                    "indentation");
        } else
        if (!isspace(c) && !in_comment) {
            break;
        }
        yaml_env_skipc(env);
    }

    if (comment_ps.s != NULL) {
        t_yaml_env_handle_comment_ps(env, comment_ps, in_new_line,
                                     &prefix_comments);
    }

    yaml_env_set_prefix_comments(env, &prefix_comments);

    return 0;
}

static bool
ps_startswith_yaml_seq_prefix(const pstream_t *ps)
{
    if (!ps_has(ps, 2)) {
        return false;
    }

    return ps->s[0] == '-' && isspace(ps->s[1]);
}

static bool
ps_startswith_yaml_key(pstream_t ps, bool must_be_variable)
{
    pstream_t key;

    if (ps_peekc(ps) == '$') {
        ps_skipc(&ps, '$');
    } else {
        if (must_be_variable) {
            return false;
        }
    }

    key = ps_get_span(&ps, &ctype_isalnum);

    if (ps_len(&key) == 0 || ps_len(&ps) == 0) {
        return false;
    }

    return ps.s[0] == ':'
        && (ps_len(&ps) == 1 || isspace(ps.s[1]));
}

/* }}} */
/* {{{ Variables */

static void
t_yaml_env_add_var(yaml_parse_t * nonnull env, const lstr_t name,
                   yaml_variable_t var)
{
    int pos;
    qv_t(yaml_variable) *vec;

    pos = qm_reserve(yaml_vars, &env->variables, &name, 0);
    if (pos & QHASH_COLLISION) {
        logger_trace(&_G.logger, 2, "add new occurrence of variable `%pL`",
                     &name);
        vec = &env->variables.values[pos & ~QHASH_COLLISION];
    } else {
        logger_trace(&_G.logger, 2, "add new variable `%pL`", &name);
        env->variables.keys[pos] = t_lstr_dup(name);
        vec = &env->variables.values[pos];
        t_qv_init(vec, 1);
    }
    qv_append(vec, var);
}

static void
yaml_env_merge_variables(yaml_parse_t * nonnull env,
                           const qm_t(yaml_vars) * nonnull vars)
{
    qm_for_each_key_value_p(yaml_vars, name, vec, vars) {
        int pos;

        logger_trace(&_G.logger, 2, "add occurrences of variable `%pL` in "
                     "including document", &name);
        pos = qm_reserve(yaml_vars, &env->variables, &name, 0);
        if (pos & QHASH_COLLISION) {
            qv_extend(&env->variables.values[pos & ~QHASH_COLLISION], vec);
        } else {
            env->variables.values[pos] = *vec;
        }
    }
}

/* Detect use of $foo in a quoted string, and add those variables in the
 * env */
/* TODO: must handle escaping! */
static void
t_yaml_env_add_variables(yaml_parse_t * nonnull env,
                         yaml_data_t * nonnull data, bool in_string)
{
    pstream_t ps;
    qh_t(lstr) variables_found;
    bool whole = false;

    assert (data->type == YAML_DATA_SCALAR
         && data->scalar.type == YAML_SCALAR_STRING);

    t_qh_init(lstr, &variables_found, 0);

    ps = ps_initlstr(&data->scalar.s);
    for (;;) {
        pstream_t ps_name;

        if (ps_skip_afterchr(&ps, '$') < 0) {
            break;
        }

        ps_name = ps_get_span(&ps, &ctype_isalnum);
        /* TODO: error on else */
        if (ps_len(&ps_name) > 0) {
            lstr_t name = LSTR_PS_V(&ps_name);

            if (name.len + 1 == data->scalar.s.len) {
                /* The whole string is this variable */
                whole = true;
            }
            qh_add(lstr, &variables_found, &name);
        }
    }

    if (qh_len(lstr, &variables_found) > 0) {
        yaml_variable_t var = {
            .data = data,
            .in_string = in_string || !whole,
        };
        qh_for_each_key(lstr, name, &variables_found) {
            t_yaml_env_add_var(env, name, var);
        }

        if (env->flags & YAML_PARSE_GEN_PRES_DATA) {
            yaml__presentation_node__t *node;

            node = t_yaml_env_pres_get_current_node(env->pres);
            node->value_with_variables = data->scalar.s;
        }
    }
}

/* Replace occurrences of $name with `value` in `data`. */
static void
t_data_set_string_variable(yaml_data_t * nonnull data,
                           const lstr_t name, const lstr_t value)
{
    t_SB_1k(buf);
    pstream_t ps;
    pstream_t sub;

    assert (data->type == YAML_DATA_SCALAR
         && data->scalar.type == YAML_SCALAR_STRING);

    ps = ps_initlstr(&data->scalar.s);

    for (;;) {
        /* copy up to next '$' */
        if (ps_get_ps_chr(&ps, '$', &sub) < 0) {
            /* no next '$', copy everything and stop */
            sb_add_ps(&buf, ps);
            break;
        }

        sb_add_ps(&buf, sub);

        ps_skipc(&ps, '$');
        if (ps_startswithlstr(&ps, name)) {
            ps_skip(&ps, name.len);
            sb_add_lstr(&buf, value);
        } else {
            sb_addc(&buf, '$');
        }
    }

    logger_trace(&_G.logger, 2, "apply replacement %pL=%pL, data value "
                 "changed from `%pL` to `%pL`", &name, &value,
                 &data->scalar.s, &buf);

    data->scalar.s = LSTR_SB_V(&buf);
}

static int
t_yaml_env_replace_variables(yaml_parse_t * nonnull env,
                             yaml_data_t * nonnull override,
                             qm_t(yaml_vars) * nonnull variables,
                             qv_t(lstr) * nullable variables_names)
{
    assert (override->type == YAML_DATA_OBJ);

    tab_for_each_ptr(pair, &override->obj->fields) {
        lstr_t name;
        int pos;

        if (!lstr_startswith(pair->key, LSTR("$"))) {
            continue;
        }

        name = LSTR_INIT_V(pair->key.s + 1, pair->key.len - 1);
        if (variables_names) {
            qv_append(variables_names, name);
        }

        pos = qm_find(yaml_vars, variables, &name);
        if (pos < 0) {
            yaml_env_set_err_at(env, &pair->key_span, YAML_ERR_BAD_KEY,
                                "unknown variable");
            return -1;
        }

        /* Replace every occurrence of the variable with the provided data. */
        tab_for_each_ptr(var, &variables->values[pos]) {
            if (var->in_string) {
                lstr_t value;

                if (pair->data.type != YAML_DATA_SCALAR)
                {
                    yaml_env_set_err_at(
                        env, &pair->data.span, YAML_ERR_WRONG_DATA,
                        "this variable can only be set with a scalar"
                    );
                    return -1;
                }

                if (pair->data.scalar.type == YAML_SCALAR_STRING) {
                    value = pair->data.scalar.s;
                } else {
                    value = yaml_span_to_lstr(&pair->data.span);
                }
                t_data_set_string_variable(var->data, name, value);
            } else {
                *var->data = pair->data;
            }
        }

        /* remove the variable from variables, to prevent matching twice,
         * and to be able to keep in the qm the variables that are still
         * active. */
        qm_del_at(yaml_vars, variables, pos);
    }

    return 0;
}

static int
t_yaml_env_parse_obj(yaml_parse_t *env, const uint32_t min_indent,
                     bool only_variables, yaml_data_t *out);

static int
t_yaml_env_handle_variables(yaml_parse_t * nonnull env,
                            const uint32_t min_indent,
                            qm_t(yaml_vars) * nonnull variables,
                            yaml__presentation_include__t * nullable pres)
{
    uint32_t cur_indent;
    yaml_data_t data;

    /* Variables are specified as an object with keys starting with '$', with
     * an indent >= to min_indent. */
    RETHROW(yaml_env_ltrim(env));
    if (ps_done(&env->ps)) {
        return 0;
    }

    cur_indent = yaml_env_get_column_nb(env);
    if (cur_indent < min_indent) {
        return 0;
    }

    if (!ps_startswith_yaml_key(env->ps, true)) {
        return 0;
    }

    RETHROW(t_yaml_env_parse_obj(env, cur_indent, true, &data));
    logger_trace(&_G.logger, 2, "parsed variable values, "
                 "%s from "YAML_POS_FMT" up to "YAML_POS_FMT,
                 yaml_data_get_type(&data, false),
                 YAML_POS_ARG(data.span.start), YAML_POS_ARG(data.span.end));

    if (pres) {
        qv_t(lstr) variables_names;

        t_qv_init(&variables_names, data.obj->fields.len);

        RETHROW(t_yaml_env_replace_variables(env, &data, variables,
                                             &variables_names));

        pres->variables = t_iop_new(yaml__presentation_variable_settings);
        pres->variables->names = IOP_TYPED_ARRAY_TAB(lstr, &variables_names);
    } else {
        RETHROW(t_yaml_env_replace_variables(env, &data, variables, NULL));
    }

    return 0;
}
/* }}} */
/* {{{ Tag */

static int
t_yaml_env_parse_tag(yaml_parse_t *env, const uint32_t min_indent,
                     yaml_data_t *out)
{
    /* a-zA-Z0-9. */
    static const ctype_desc_t ctype_tag = { {
        0x00000000, 0x03ff4000, 0x07fffffe, 0x07fffffe,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    } };
    yaml_pos_t tag_pos_start = yaml_env_get_pos(env);
    yaml_pos_t tag_pos_end;
    pstream_t tag;

    assert (ps_peekc(env->ps) == '!');
    yaml_env_skipc(env);

    if (!isalpha(ps_peekc(env->ps))) {
        return yaml_env_set_err(env, YAML_ERR_INVALID_TAG,
                                "must start with a letter");
    }

    tag = ps_get_span(&env->ps, &ctype_tag);
    if (!isspace(ps_peekc(env->ps))) {
        return yaml_env_set_err(env, YAML_ERR_INVALID_TAG,
                                "must only contain alphanumeric characters");
    }
    tag_pos_end = yaml_env_get_pos(env);

    RETHROW(t_yaml_env_parse_data(env, min_indent, out));
    if (out->tag.s) {
        return yaml_env_set_err(env, YAML_ERR_WRONG_OBJECT,
                                "two tags have been declared");
    }

    out->tag = LSTR_PS_V(&tag);
    out->span.start = tag_pos_start;
    out->tag_span = t_new(yaml_span_t, 1);
    yaml_span_init(out->tag_span, env, tag_pos_start, tag_pos_end);

    return 0;
}

static bool has_inclusion_loop(const yaml_parse_t * nonnull env,
                               const lstr_t newfile)
{
    do {
        if (lstr_equal(env->fullpath, newfile)) {
            return true;
        }
        env = env->included ? env->included->parent : NULL;
    } while (env);

    return false;
}

static int t_yaml_env_do_include(yaml_parse_t * nonnull env, bool raw,
                                 yaml_data_t * nonnull data,
                                 qm_t(yaml_vars) * nonnull variables)
{
    yaml_parse_t *subfile = NULL;
    yaml_data_t subdata;
    char dirpath[PATH_MAX];
    SB_1k(err);

    RETHROW(yaml_env_ltrim(env));

    if (data->type != YAML_DATA_SCALAR
    ||  data->scalar.type != YAML_SCALAR_STRING)
    {
        sb_setf(&err, "!%pL can only be used with strings", &data->tag);
        goto err;
    }

    path_dirname(dirpath, PATH_MAX, env->fullpath.s ?: "");

    if (raw) {
        logger_trace(&_G.logger, 2, "copying raw subfile %pL",
                     &data->scalar.s);
    } else {
        logger_trace(&_G.logger, 2, "parsing subfile %pL", &data->scalar.s);
    }

    subfile = t_yaml_parse_new(
        YAML_PARSE_GEN_PRES_DATA
      | YAML_PARSE_ALLOW_UNBOUND_VARIABLES
    );
    if (t_yaml_parse_attach_file(subfile, t_fmt("%pL", &data->scalar.s),
                                 dirpath, &err) < 0)
    {
        goto err;

    }
    if (has_inclusion_loop(env, subfile->fullpath)) {
        sb_sets(&err, "inclusion loop detected");
        goto err;
    }

    subfile->included = t_new(yaml_included_file_t, 1);
    subfile->included->parent = env;
    subfile->included->data = *data;
    qv_append(&env->subfiles, subfile);

    if (raw) {
        yaml_data_set_string(&subdata, subfile->file_contents);
    } else {
        if (t_yaml_parse(subfile, &subdata, &err) < 0) {
            /* no call to yaml_env_set_err, because the generated error message
             * will already have all the including details. */
            env->err = subfile->err;
            return -1;
        }
    }

    *variables = subfile->variables;

    if (env->pres) {
        yaml__presentation_include__t *inc;

        inc = t_iop_new(yaml__presentation_include);
        inc->include_presentation = data->presentation;
        inc->path = data->scalar.s;
        inc->raw = raw;
        t_yaml_data_get_presentation(&subdata, &inc->document_presentation);

        /* XXX: create a new presentation node for subdata, that indicates it
         * is included. We should not modify the existing presentation node
         * (if it exists), as it indicates the presentation of the subdata
         * in the subfile, and was saved in "inc->presentation". */
        subdata.presentation = t_iop_new(yaml__presentation_node);
        subdata.presentation->included = inc;
    }

    *data = subdata;
    return 0;

  err:
    yaml_env_set_err_at(env, &data->span, YAML_ERR_INVALID_INCLUDE,
                        t_fmt("%pL", &err));
    return -1;
}

static int t_yaml_env_handle_override(yaml_parse_t *env,
                                      const uint32_t min_indent,
                                      yaml_data_t *out);

static int
t_yaml_env_handle_include(yaml_parse_t * nonnull env,
                          const uint32_t min_indent,
                          yaml_data_t * nonnull data)
{
    yaml__presentation_include__t *pres;
    qm_t(yaml_vars) vars;
    bool raw;

    if (lstr_equal(data->tag, LSTR("include"))) {
        raw = false;
    } else
    if (lstr_equal(data->tag, LSTR("includeraw"))) {
        raw = true;
    } else {
        return 0;
    }

    /* Parse and retrieve the included AST, and get the associated variables.
     */
    RETHROW(t_yaml_env_do_include(env, raw, data, &vars));
    pres = data->presentation ? data->presentation->included : NULL;

    /* Parse and apply variables. */
    RETHROW(t_yaml_env_handle_variables(env, min_indent, &vars, pres));

    /* Parse and merge overrides. */
    RETHROW(t_yaml_env_handle_override(env, min_indent, data));

    /* Save remaining variables into current variables for the document. */
    yaml_env_merge_variables(env, &vars);

    return 0;
}

/* }}} */
/* {{{ Seq */

/** Get the presentation stored for the next node, and save in "last_node"
 * to ensure inline presentation data uses this node. */
static void
yaml_env_pop_next_node(yaml_parse_t * nonnull env,
                       yaml__presentation_node__t * nullable * nonnull node)
{
    *node = env->pres->next_node;
    env->pres->next_node = NULL;
    env->pres->last_node = node;
}

static int t_yaml_env_parse_seq(yaml_parse_t *env, const uint32_t min_indent,
                                yaml_data_t *out)
{
    qv_t(yaml_data) datas;
    qv_t(yaml_pres_node) pres;
    yaml_pos_t pos_end = {0};

    t_qv_init(&datas, 0);
    t_qv_init(&pres, 0);

    assert (ps_startswith_yaml_seq_prefix(&env->ps));
    yaml_env_start_data(env, YAML_DATA_SEQ, out);

    for (;;) {
        yaml__presentation_node__t *node = NULL;
        yaml_data_t *elem;
        uint32_t last_indent;

        RETHROW(yaml_env_ltrim(env));
        if (env->pres) {
            yaml_env_pop_next_node(env, &node);
        }

        /* skip '-' */
        yaml_env_skipc(env);

        elem = qv_growlen(&datas, 1);
        RETHROW(t_yaml_env_parse_data(env, min_indent + 1, elem));
        RETHROW(yaml_env_ltrim(env));

        pos_end = elem->span.end;
        qv_append(&pres, node);

        if (ps_done(&env->ps)) {
            break;
        }

        last_indent = yaml_env_get_column_nb(env);
        if (last_indent < min_indent) {
            /* we go down on indent, so the seq is over */
            break;
        }
        if (last_indent > min_indent) {
            return yaml_env_set_err(env, YAML_ERR_WRONG_INDENT,
                                    "line not aligned with current sequence");
        } else
        if (!ps_startswith_yaml_seq_prefix(&env->ps)) {
            return yaml_env_set_err(env, YAML_ERR_WRONG_DATA,
                                    "expected another element of sequence");
        }
    }

    yaml_env_end_data_with_pos(env, pos_end, out);
    out->seq = t_new(yaml_seq_t, 1);
    out->seq->datas = datas;
    out->seq->pres_nodes = pres;

    return 0;
}

/* }}} */
/* {{{ Obj */

static int
yaml_env_parse_key(yaml_parse_t * nonnull env, lstr_t * nonnull key,
                   yaml_span_t * nonnull key_span,
                   yaml__presentation_node__t * nonnull * nullable node)
{
    pstream_t ps_key;
    yaml_pos_t key_pos_start = yaml_env_get_pos(env);
    const char *start;

    RETHROW(yaml_env_ltrim(env));
    if (env->pres && node) {
        yaml_env_pop_next_node(env, node);
    }

    start = env->ps.s;
    if (ps_peekc(env->ps) == '$') {
        IGNORE(ps_skipc(&env->ps, '$'));
    }
    ps_skip_span(&env->ps, &ctype_isalnum);

    ps_key = ps_initptr(start, env->ps.s);
    yaml_span_init(key_span, env, key_pos_start, yaml_env_get_pos(env));

    if (ps_len(&ps_key) == 0) {
        return yaml_env_set_err(env, YAML_ERR_BAD_KEY,
                                "only alpha-numeric characters allowed");
    } else
    if (ps_getc(&env->ps) != ':') {
        return yaml_env_set_err(env, YAML_ERR_BAD_KEY, "missing colon");
    }

    *key = LSTR_PS_V(&ps_key);

    return 0;
}

static int
t_yaml_env_parse_obj(yaml_parse_t *env, const uint32_t min_indent,
                     bool only_variables, yaml_data_t *out)
{
    qv_t(yaml_key_data) fields;
    yaml_pos_t pos_end = {0};
    qh_t(lstr) keys_hash;

    t_qv_init(&fields, 0);
    t_qh_init(lstr, &keys_hash, 0);

    yaml_env_start_data(env, YAML_DATA_OBJ, out);

    for (;;) {
        lstr_t key;
        yaml_key_data_t *kd;
        uint32_t last_indent;
        yaml_span_t key_span;
        yaml__presentation_node__t *node;

        if (only_variables) {
            RETHROW(yaml_env_ltrim(env));
            if (ps_peekc(env->ps) != '$') {
                /* If only_variables is true, we only want to parse variable
                 * sets, so as soon as we don't seem to be in this context,
                 * we stop. */
                break;
            }
        }

        RETHROW(yaml_env_parse_key(env, &key, &key_span, &node));
        if (!only_variables && lstr_startswith(key, LSTR("$"))) {
            return yaml_env_set_err_at(env, &key_span, YAML_ERR_BAD_KEY,
                                       "cannot specify a variable value in "
                                       "this context");
        }

        kd = qv_growlen0(&fields, 1);
        kd->key = key;
        kd->key_span = key_span;
        if (qh_add(lstr, &keys_hash, &kd->key) < 0) {
            return yaml_env_set_err_at(env, &key_span, YAML_ERR_BAD_KEY,
                                       "key is already declared in the "
                                       "object");
        }

        /* XXX: This is a hack to handle the tricky case where a sequence
         * has the same indentation as the key:
         *  a:
         *  - 1
         *  - 2
         * This syntax is valid YAML, but breaks the otherwise valid contract
         * that a subdata always has a strictly greater indentation level than
         * its containing data.
         */
        RETHROW(yaml_env_ltrim(env));

        if (ps_startswith_yaml_seq_prefix(&env->ps)) {
            RETHROW(t_yaml_env_parse_data(env, min_indent, &kd->data));
        } else {
            RETHROW(t_yaml_env_parse_data(env, min_indent + 1, &kd->data));
        }

        pos_end = kd->data.span.end;
        kd->key_presentation = node;
        RETHROW(yaml_env_ltrim(env));

        if (ps_done(&env->ps)) {
            break;
        }

        last_indent = yaml_env_get_column_nb(env);
        if (last_indent < min_indent) {
            /* we go down on indent, so the obj is over */
            break;
        }
        if (last_indent > min_indent) {
            return yaml_env_set_err(env, YAML_ERR_WRONG_INDENT,
                                    "line not aligned with current object");
        }
    }

    yaml_env_end_data_with_pos(env, pos_end, out);
    out->obj = t_new(yaml_obj_t, 1);
    out->obj->fields = fields;

    return 0;
}

/* }}} */
/* {{{ Scalar */

static pstream_t yaml_env_get_scalar_ps(yaml_parse_t * nonnull env,
                                        bool in_flow)
{
    /* '\n' and '#' */
    static const ctype_desc_t ctype_scalarend = { {
        0x00000400, 0x00000008, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    } };
    /* '\n', '#', '{, '[', '}', ']' or ',' */
    static const ctype_desc_t ctype_scalarflowend = { {
        0x00000400, 0x00001008, 0x28000000, 0x28000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    } };
    pstream_t scalar;

    if (in_flow) {
        scalar = ps_get_cspan(&env->ps, &ctype_scalarflowend);
    } else {
        scalar = ps_get_cspan(&env->ps, &ctype_scalarend);
    }

    /* need to rtrim to remove extra spaces */
    ps_rtrim(&scalar);

    /* Position the env ps to the end of the trimmed scalar ps, so that
     * the span can be correctly computed. */
    env->ps.s = scalar.s_end;

    return scalar;
}

static int
t_yaml_env_parse_quoted_string(yaml_parse_t *env, yaml_data_t *out)
{
    int line_nb = 0;
    int col_nb = 0;
    sb_t buf;
    parse_str_res_t res;

    assert (ps_peekc(env->ps) == '"');
    yaml_env_skipc(env);

    t_sb_init(&buf, 128);
    res = parse_quoted_string(&env->ps, &buf, &line_nb, &col_nb, '"');
    switch (res) {
      case PARSE_STR_ERR_UNCLOSED:
        return yaml_env_set_err(env, YAML_ERR_BAD_STRING,
                                "missing closing '\"'");
      case PARSE_STR_ERR_EXP_SMTH:
        return yaml_env_set_err(env, YAML_ERR_BAD_STRING,
                                "invalid backslash");
      case PARSE_STR_OK:
        yaml_env_end_data(env, out);
        out->scalar.type = YAML_SCALAR_STRING;
        out->scalar.s = LSTR_SB_V(&buf);
        return 0;
    }

    assert (false);
    return yaml_env_set_err(env, YAML_ERR_BAD_STRING, "invalid string");
}

static int
yaml_parse_special_scalar(lstr_t line, yaml_scalar_t *out)
{
    if (lstr_equal(line, LSTR("~"))
    ||  lstr_ascii_iequal(line, LSTR("null")))
    {
        out->type = YAML_SCALAR_NULL;
        return 0;
    } else
    if (lstr_ascii_iequal(line, LSTR("true"))) {
        out->type = YAML_SCALAR_BOOL;
        out->b = true;
        return 0;
    } else
    if (lstr_ascii_iequal(line, LSTR("false"))) {
        out->type = YAML_SCALAR_BOOL;
        out->b = false;
        return 0;
    } else
    if (lstr_ascii_iequal(line, LSTR("-.inf"))) {
        out->type = YAML_SCALAR_DOUBLE;
        out->d = -INFINITY;
        return 0;
    } else
    if (lstr_ascii_iequal(line, LSTR(".inf"))) {
        out->type = YAML_SCALAR_DOUBLE;
        out->d = INFINITY;
        return 0;
    } else
    if (lstr_ascii_iequal(line, LSTR(".nan"))) {
        out->type = YAML_SCALAR_DOUBLE;
        out->d = NAN;
        return 0;
    }

    return -1;
}

static int
yaml_parse_numeric_scalar(lstr_t line, yaml_scalar_t *out)
{
    double d;

    if (line.s[0] == '-') {
        int64_t i;

        if (lstr_to_int64(line, &i) == 0) {
            if (i >= 0) {
                /* This can happen for -0 for example. Force to use UINT
                 * in that case, to make sure INT is only used for < 0. */
                out->type = YAML_SCALAR_UINT;
                out->u = i;
            } else {
                out->type = YAML_SCALAR_INT;
                out->i = i;
            }
            return 0;
        }
    } else {
        uint64_t u;

        if (lstr_to_uint64(line, &u) == 0) {
            out->type = YAML_SCALAR_UINT;
            out->u = u;
            return 0;
        }
    }

    if (lstr_to_double(line, &d) == 0) {
        out->type = YAML_SCALAR_DOUBLE;
        out->d = d;
        return 0;
    }

    return -1;
}

static int t_yaml_env_parse_scalar(yaml_parse_t *env, bool in_flow,
                                   yaml_data_t *out)
{
    lstr_t line;
    pstream_t ps_line;

    yaml_env_start_data(env, YAML_DATA_SCALAR, out);
    if (ps_peekc(env->ps) == '"') {
        RETHROW(t_yaml_env_parse_quoted_string(env, out));
        t_yaml_env_add_variables(env, out, true);

        return 0;
    }

    /* get scalar string, ie up to newline or comment, or ']' or ',' for flow
     * context */
    ps_line = yaml_env_get_scalar_ps(env, in_flow);
    if (ps_len(&ps_line) == 0) {
        return yaml_env_set_err(env, YAML_ERR_MISSING_DATA,
                                "unexpected character");
    }

    line = LSTR_PS_V(&ps_line);
    yaml_env_end_data(env, out);

    /* special strings */
    if (yaml_parse_special_scalar(line, &out->scalar) >= 0) {
        return 0;
    }

    /* try to parse it as a int/uint or float */
    if (yaml_parse_numeric_scalar(line, &out->scalar) >= 0) {
        return 0;
    }

    /* If all else fail, it is a string. */
    out->scalar.type = YAML_SCALAR_STRING;
    out->scalar.s = line;

    t_yaml_env_add_variables(env, out, false);

    return 0;
}

/* }}} */
/* {{{ Flow seq */

static int t_yaml_env_parse_flow_key_data(yaml_parse_t * nonnull env,
                                          yaml_key_data_t * nonnull out);

static void
t_yaml_env_build_implicit_obj(yaml_parse_t * nonnull env,
                              yaml_key_data_t * nonnull kd,
                              yaml_data_t * nonnull out)
{
    qv_t(yaml_key_data) fields;

    t_qv_init(&fields, 1);
    qv_append(&fields, *kd);

    yaml_env_start_data_with_pos(env, YAML_DATA_OBJ, kd->key_span.start, out);
    yaml_env_end_data_with_pos(env, kd->data.span.end, out);
    out->obj = t_new(yaml_obj_t, 1);
    out->obj->fields = fields;
}

/* A flow sequence begins with '[', ends with ']' and elements are separated
 * by ','.
 * Inside a flow sequence, block types (ie using indentation) are forbidden,
 * and values can only be:
 *  - a scalar
 *  - a value pair: `a: b`
 *  - a flow object: `{ ... }`
 *  - a flow seq: `[ ... ]`
 */
static int
t_yaml_env_parse_flow_seq(yaml_parse_t *env, yaml_data_t *out)
{
    qv_t(yaml_data) datas;

    t_qv_init(&datas, 0);

    /* skip '[' */
    assert (ps_peekc(env->ps) == '[');
    yaml_env_start_data(env, YAML_DATA_SEQ, out);
    yaml_env_skipc(env);

    for (;;) {
        yaml_key_data_t kd;

        RETHROW(yaml_env_ltrim(env));
        if (ps_peekc(env->ps) == ']') {
            yaml_env_skipc(env);
            goto end;
        }

        RETHROW(t_yaml_env_parse_flow_key_data(env, &kd));
        if (kd.key.s) {
            yaml_data_t obj;

            t_yaml_env_build_implicit_obj(env, &kd, &obj);
            qv_append(&datas, obj);
        } else {
            qv_append(&datas, kd.data);
        }

        RETHROW(yaml_env_ltrim(env));
        switch (ps_peekc(env->ps)) {
          case ']':
            yaml_env_skipc(env);
            goto end;
          case ',':
            yaml_env_skipc(env);
            break;
          default:
            return yaml_env_set_err(env, YAML_ERR_WRONG_DATA,
                                    "expected another element of sequence");
        }
    }

  end:
    yaml_env_end_data(env, out);
    out->seq = t_new(yaml_seq_t, 1);
    out->seq->datas = datas;

    return 0;
}

/* }}} */
/* {{{ Flow obj */

/* A flow sequence begins with '{', ends with '}' and elements are separated
 * by ','.
 * Inside a flow sequence, block types (ie using indentation) are forbidden,
 * and only value pairs are allowed: `key: <flow_data>`.
 */
static int
t_yaml_env_parse_flow_obj(yaml_parse_t *env, yaml_data_t *out)
{
    qv_t(yaml_key_data) fields;
    qh_t(lstr) keys_hash;

    t_qv_init(&fields, 0);
    t_qh_init(lstr, &keys_hash, 0);

    /* skip '{' */
    assert (ps_peekc(env->ps) == '{');
    yaml_env_start_data(env, YAML_DATA_OBJ, out);
    yaml_env_skipc(env);

    for (;;) {
        yaml_key_data_t kd;

        RETHROW(yaml_env_ltrim(env));
        if (ps_peekc(env->ps) == '}') {
            yaml_env_skipc(env);
            goto end;
        }

        RETHROW(t_yaml_env_parse_flow_key_data(env, &kd));
        if (!kd.key.s) {
            return yaml_env_set_err_at(env, &kd.data.span,
                                       YAML_ERR_WRONG_DATA,
                                       "only key-value mappings are allowed "
                                       "inside an object");
        } else
        if (qh_add(lstr, &keys_hash, &kd.key) < 0) {
            return yaml_env_set_err_at(env, &kd.key_span, YAML_ERR_BAD_KEY,
                                       "key is already declared in the "
                                       "object");
        }
        qv_append(&fields, kd);

        RETHROW(yaml_env_ltrim(env));
        switch (ps_peekc(env->ps)) {
          case '}':
            yaml_env_skipc(env);
            goto end;
          case ',':
            yaml_env_skipc(env);
            break;
          default:
            return yaml_env_set_err(env, YAML_ERR_WRONG_DATA,
                                    "expected another element of object");
        }
    }

  end:
    yaml_env_end_data(env, out);
    out->obj = t_new(yaml_obj_t, 1);
    out->obj->fields = fields;

    return 0;
}

/* }}} */
/* {{{ Flow key-data */

static int t_yaml_env_parse_flow_key_val(yaml_parse_t *env,
                                         yaml_key_data_t *out)
{
    yaml_key_data_t kd;

    RETHROW(yaml_env_parse_key(env, &out->key, &out->key_span, NULL));
    if (lstr_startswith(out->key, LSTR("$"))) {
        return yaml_env_set_err_at(env, &out->key_span, YAML_ERR_BAD_KEY,
                                   "cannot specify a variable value in "
                                   "this context");
    }

    RETHROW(yaml_env_ltrim(env));
    RETHROW(t_yaml_env_parse_flow_key_data(env, &kd));
    if (kd.key.s) {
        yaml_span_t span;

        /* This means the value was a key val mapping:
         *   a: b: c.
         * Place the ps on the end of the second key, to point to the second
         * colon. */
        span = kd.key_span;
        span.start = span.end;
        span.end.col_nb++;
        span.end.s++;
        return yaml_env_set_err_at(env, &span, YAML_ERR_WRONG_DATA,
                                   "unexpected colon");
    } else {
        out->data = kd.data;
    }

    return 0;
}

/* As inside a flow context, implicit key-value mappings are allowed, It is
 * easier to return a key_data object:
 *  * if a key:value mapping is parsed, a yaml_key_data_t object is returned.
 *  * otherwise, only out->data is filled, and out->key.s is set to NULL.
 */
static int t_yaml_env_parse_flow_key_data(yaml_parse_t *env,
                                          yaml_key_data_t *out)
{
    p_clear(out, 1);

    RETHROW(yaml_env_ltrim(env));
    if (ps_done(&env->ps)) {
        return yaml_env_set_err(env, YAML_ERR_MISSING_DATA,
                                "unexpected end of line");
    }

    if (ps_startswith_yaml_key(env->ps, false)) {
        RETHROW(t_yaml_env_parse_flow_key_val(env, out));
        goto end;
    }

    out->key = LSTR_NULL_V;
    if (ps_peekc(env->ps) == '[') {
        RETHROW(t_yaml_env_parse_flow_seq(env, &out->data));
    } else
    if (ps_peekc(env->ps) == '{') {
        RETHROW(t_yaml_env_parse_flow_obj(env, &out->data));
    } else {
        RETHROW(t_yaml_env_parse_scalar(env, true, &out->data));
    }

  end:
    log_new_data(&out->data);
    return 0;
}

/* }}} */
/* {{{ Override */

/* Some data can have an "override" that modifies the previously parsed
 * content.
 * This is for example true for !include, and would be true for anchors
 * if implemented.
 * These overrides are specified by defining an object on a greater
 * indentation than the including object:
 *
 * - !include foo.yml
 *   a: 2
 *   b: 5
 *
 * or
 *
 * key: !include bar.yml
 *   c: ~
 *
 * All the fields of the override are then merged in the modified data.
 * The merge strategy is this one (only merge with the same yaml data type is
 * allowed):
 *  * for scalars, the override overwrites the original data.
 *  * for sequences, all data from the override are added.
 *  * for obj, matched keys means recursing the merge in the inner datas, and
 *    unmatched keys are added.
 */

/* {{{ Merging */

static void
yaml_pres_override_add_node(const lstr_t path,
                            const yaml_data_t * nullable data,
                            qv_t(override_nodes) * nonnull nodes)

{
    yaml__presentation_override_node__t *node;

    node = qv_growlen(nodes, 1);
    iop_init(yaml__presentation_override_node, node);
    node->path = path;
    if (data) {
        node->original_data = t_iop_new(yaml__data);
        t_yaml_data_to_iop(data, node->original_data);
    }
}

static int
t_yaml_env_merge_data(yaml_parse_t * nonnull env,
                      const yaml_data_t * nonnull override,
                      yaml_presentation_override_t * nullable pres,
                      yaml_data_t * nonnull data);

static int
t_yaml_env_merge_key_data(yaml_parse_t * nonnull env,
                          const yaml_key_data_t * nonnull override,
                          yaml_presentation_override_t * nullable pres,
                          yaml_obj_t * nonnull obj)
{
    int prev_len = 0;

    tab_for_each_ptr(data_pair, &obj->fields) {
        if (lstr_equal(data_pair->key, override->key)) {
            if (pres) {
                prev_len = pres->path.len;

                sb_addf(&pres->path, ".%pL", &data_pair->key);
            }

            /* key found, recurse the merging of the inner data */
            RETHROW(t_yaml_env_merge_data(env, &override->data,
                                          pres, &data_pair->data));
            if (pres) {
                sb_clip(&pres->path, prev_len);
            }
            return 0;
        }
    }

    /* key not found, add the pair to the object. */
    logger_trace(&_G.logger, 2,
                 "merge new key from "YAML_POS_FMT" up to "YAML_POS_FMT,
                 YAML_POS_ARG(override->key_span.start),
                 YAML_POS_ARG(override->key_span.end));
    qv_append(&obj->fields, *override);

    if (pres) {
        lstr_t path = t_lstr_fmt("%pL.%pL", &pres->path, &override->key);

        yaml_pres_override_add_node(path, NULL, &pres->nodes);
    }

    return 0;
}

static int t_yaml_env_merge_obj(yaml_parse_t * nonnull env,
                                const yaml_obj_t * nonnull override,
                                yaml_presentation_override_t * nullable pres,
                                yaml_obj_t * nonnull obj)
{
    /* XXX: O(n^2), not great but normal usecase would never override
     * every key of a huge object, so the tradeoff is fine.
     */
    tab_for_each_ptr(pair, &override->fields) {
        if (!lstr_startswith(pair->key, LSTR("$"))) {
            RETHROW(t_yaml_env_merge_key_data(env, pair, pres, obj));
        }
    }

    return 0;
}

static int yaml_env_merge_seq(yaml_parse_t * nonnull env,
                              const yaml_seq_t * nonnull override,
                              const yaml_span_t * nonnull span,
                              yaml_presentation_override_t * nullable pres,
                              yaml_seq_t * nonnull seq)
{
    logger_trace(&_G.logger, 2,
                 "merging seq from "YAML_POS_FMT" up to "YAML_POS_FMT
                 " by appending its datas", YAML_POS_ARG(span->start),
                 YAML_POS_ARG(span->end));

    if (pres) {
        int len = seq->datas.len;
        for (int i = 0; i < override->datas.len; i++) {
            lstr_t path = t_lstr_fmt("%pL[%d]", &pres->path, len + i);

            yaml_pres_override_add_node(path, NULL, &pres->nodes);
        }
    }

    /* Until a proper syntax is found, seq merge are only additive */
    qv_extend(&seq->datas, &override->datas);
    qv_extend(&seq->pres_nodes, &override->pres_nodes);

    return 0;
}

static void
t_yaml_merge_scalar(const yaml_data_t * nonnull override,
                    yaml_presentation_override_t * nullable pres,
                    yaml_data_t * nonnull out)
{
    if (pres) {
        lstr_t path = t_lstr_dup(LSTR_SB_V(&pres->path));

        yaml_pres_override_add_node(path, out, &pres->nodes);
    }

    logger_trace(&_G.logger, 2,
                 "merging scalar from "YAML_POS_FMT" up to "YAML_POS_FMT,
                 YAML_POS_ARG(override->span.start),
                 YAML_POS_ARG(override->span.end));
    *out = *override;
}

static int
t_yaml_env_merge_data(yaml_parse_t * nonnull env,
                      const yaml_data_t * nonnull override,
                      yaml_presentation_override_t * nullable pres,
                      yaml_data_t * nonnull data)
{
    if (data->type != override->type) {
        const char *msg;

        /* XXX: This could be allowed, and implemented by completely replacing
         * the overridden data with the overriding one. However, the use-cases
         * are not clear, and it could hide errors, so reject it until a
         * valid use-case is found. */
        msg = t_fmt("overridden data is %s and not %s",
                    yaml_data_get_data_type(data),
                    yaml_data_get_data_type(override));
        return yaml_env_set_err_at(env, &override->span,
                                   YAML_ERR_INVALID_OVERRIDE, msg);
    }

    switch (data->type) {
      case YAML_DATA_SCALAR: {
        int prev_len = 0;

        if (pres) {
            prev_len = pres->path.len;

            sb_addc(&pres->path, '!');
        }

        t_yaml_merge_scalar(override, pres, data);

        if (pres) {
            sb_clip(&pres->path, prev_len);
        }
      } break;
      case YAML_DATA_SEQ:
        RETHROW(yaml_env_merge_seq(env, override->seq, &override->span,
                                   pres, data->seq));
        break;
      case YAML_DATA_OBJ:
        RETHROW(t_yaml_env_merge_obj(env, override->obj, pres, data->obj));
        break;
    }

    return 0;
}

/* }}} */
/* {{{ Override */

static int t_yaml_env_handle_override(yaml_parse_t * nonnull env,
                                      const uint32_t min_indent,
                                      yaml_data_t * nonnull out)
{
    uint32_t cur_indent;
    yaml_data_t override;
    yaml_presentation_override_t *pres = NULL;

    /* To be an override, we want an object starting with an indent greater
     * than the min_indent. Not matching means no override, so we return
     * immediately. */
    RETHROW(yaml_env_ltrim(env));
    if (ps_done(&env->ps)) {
        return 0;
    }

    cur_indent = yaml_env_get_column_nb(env);
    if (cur_indent < min_indent) {
        return 0;
    }

    /* TODO: technically, we could allow override of any type of data, not
     * just obj, by removing this check. */
    if (!ps_startswith_yaml_key(env->ps, false)) {
        return 0;
    }

    RETHROW(t_yaml_env_parse_obj(env, cur_indent, false, &override));
    logger_trace(&_G.logger, 2,
                 "parsed override, %s from "YAML_POS_FMT" up to "YAML_POS_FMT,
                 yaml_data_get_type(&override, false),
                 YAML_POS_ARG(override.span.start),
                 YAML_POS_ARG(override.span.end));

    if (env->flags & YAML_PARSE_GEN_PRES_DATA) {
        pres = t_new(yaml_presentation_override_t, 1);
        t_qv_init(&pres->nodes, 0);
        t_sb_init(&pres->path, 1024);
    }

    RETHROW(t_yaml_env_merge_data(env, &override, pres, out));

    if (pres) {
        assert (out->presentation && out->presentation->included);
        out->presentation->included->override
            = t_presentation_override_to_iop(pres, &override);
    }

    return 0;
}

/* }}} */
/* }}} */
/* {{{ Data */

static int t_yaml_env_parse_data(yaml_parse_t *env, const uint32_t min_indent,
                                 yaml_data_t *out)
{
    uint32_t cur_indent;

    RETHROW(yaml_env_ltrim(env));
    if (ps_done(&env->ps)) {
        return yaml_env_set_err(env, YAML_ERR_MISSING_DATA,
                                "unexpected end of line");
    }

    cur_indent = yaml_env_get_column_nb(env);
    if (cur_indent < min_indent) {
        return yaml_env_set_err(env, YAML_ERR_WRONG_INDENT,
                                "missing element");
    }

    if (ps_peekc(env->ps) == '!') {
        RETHROW(t_yaml_env_parse_tag(env, min_indent, out));
        RETHROW(t_yaml_env_handle_include(env, min_indent + 1, out));
    } else
    if (ps_startswith_yaml_seq_prefix(&env->ps)) {
        RETHROW(t_yaml_env_parse_seq(env, cur_indent, out));
    } else
    if (ps_peekc(env->ps) == '[') {
        RETHROW(t_yaml_env_parse_flow_seq(env, out));
        if (out->seq->datas.len > 0) {
            t_yaml_env_pres_set_flow_mode(env);
        }
    } else
    if (ps_peekc(env->ps) == '{') {
        RETHROW(t_yaml_env_parse_flow_obj(env, out));
        if (out->obj->fields.len > 0) {
            t_yaml_env_pres_set_flow_mode(env);
        }
    } else
    if (ps_startswith_yaml_key(env->ps, false)) {
        RETHROW(t_yaml_env_parse_obj(env, cur_indent, false, out));
    } else {
        RETHROW(t_yaml_env_parse_scalar(env, false, out));
    }

    log_new_data(out);
    return 0;
}

/* }}} */
/* }}} */
/* {{{ Generate presentations */

qvector_t(pres_mapping, yaml__presentation_node_mapping__t);

static void
add_mapping(const sb_t * nonnull sb_path,
            const yaml__presentation_node__t * nonnull node,
            qv_t(pres_mapping) * nonnull out)
{
    yaml__presentation_node_mapping__t *mapping;

    mapping = qv_growlen(out, 1);
    iop_init(yaml__presentation_node_mapping, mapping);

    mapping->path = t_lstr_dup(LSTR_SB_V(sb_path));
    mapping->node = *node;
}

static void
t_yaml_add_pres_mappings(const yaml_data_t * nonnull data, sb_t *path,
                         qv_t(pres_mapping) * nonnull mappings)
{
    if (data->presentation) {
        int prev_len = path->len;

        sb_addc(path, '!');
        add_mapping(path, data->presentation, mappings);
        sb_clip(path, prev_len);

        if (data->presentation->included) {
            return;
        }
    }

    switch (data->type) {
      case YAML_DATA_SCALAR:
        break;

      case YAML_DATA_SEQ: {
        int prev_len = path->len;

        tab_enumerate_ptr(pos, val, &data->seq->datas) {
            sb_addf(path, "[%d]", pos);
            if (pos < data->seq->pres_nodes.len) {
                yaml__presentation_node__t *node;

                node = data->seq->pres_nodes.tab[pos];
                if (node) {
                    add_mapping(path, node, mappings);
                }
            }
            t_yaml_add_pres_mappings(val, path, mappings);
            sb_clip(path, prev_len);
        }
      } break;

      case YAML_DATA_OBJ: {
        int prev_len = path->len;

        tab_for_each_ptr(kv, &data->obj->fields) {
            sb_addf(path, ".%pL", &kv->key);
            if (kv->key_presentation) {
                add_mapping(path, kv->key_presentation, mappings);
            }
            t_yaml_add_pres_mappings(&kv->data, path, mappings);
            sb_clip(path, prev_len);
        }
      } break;
    }
}

/* }}} */
/* {{{ Parser public API */

yaml_parse_t *t_yaml_parse_new(int flags)
{
    yaml_parse_t *env;

    env = t_new(yaml_parse_t, 1);
    env->flags = flags;
    t_sb_init(&env->err, 1024);
    t_qv_init(&env->subfiles, 0);
    t_qm_init(yaml_vars, &env->variables, 0);

    return env;
}

void yaml_parse_delete(yaml_parse_t **env)
{
    if (!(*env)) {
        return;
    }
    lstr_wipe(&(*env)->file_contents);
    qv_deep_clear(&(*env)->subfiles, yaml_parse_delete);
}

void yaml_parse_attach_ps(yaml_parse_t *env, pstream_t ps)
{
    env->ps = ps;
    env->pos_newline = ps.s;
    env->line_number = 1;
}

int
t_yaml_parse_attach_file(yaml_parse_t *env, const char *filepath,
                         const char *dirpath, sb_t *err)
{
    char fullpath[PATH_MAX];

    path_extend(fullpath, dirpath ?: "", "%s", filepath);
    path_simplify(fullpath);

    /* detect includes that are not contained in the same directory */
    if (dirpath) {
        char relative_path[PATH_MAX];

        /* to work with path_relative_to, dirpath must end with a '/' */
        dirpath = t_fmt("%s/", dirpath);

        path_relative_to(relative_path, dirpath, fullpath);
        if (lstr_startswith(LSTR(relative_path), LSTR(".."))) {
            sb_setf(err, "cannot include subfile `%s`: "
                    "only includes contained in the directory of the "
                    "including file are allowed", filepath);
            return -1;
        }
    }

    if (lstr_init_from_file(&env->file_contents, fullpath, PROT_READ,
                            MAP_SHARED) < 0)
    {
        sb_setf(err, "cannot read file %s: %m", filepath);
        return -1;
    }

    env->filepath = t_strdup(filepath);
    env->fullpath = t_lstr_dup(LSTR(fullpath));
    yaml_parse_attach_ps(env, ps_initlstr(&env->file_contents));

    return 0;
}

static void
set_unbound_variables_err(yaml_parse_t *env)
{
    SB_1k(buf);

    /* build list of unbound variable names */
    qm_for_each_key(yaml_vars, name, &env->variables) {
        if (buf.len > 0) {
            sb_adds(&buf, ", ");
        }
        sb_add_lstr(&buf, name);
    }

    /* TODO: maybe pretty printing the locations of the unbound variables
     * would be useful */
    sb_setf(&env->err, "the document is invalid: "
            "there are unbound variables: %pL", &buf);
}

int t_yaml_parse(yaml_parse_t *env, yaml_data_t *out, sb_t *out_err)
{
    pstream_t saved_ps = env->ps;
    int res = 0;

    if (env->flags & YAML_PARSE_GEN_PRES_DATA) {
        env->pres = t_new(yaml_env_presentation_t, 1);
    }

    assert (env->ps.s && "yaml_parse_attach_ps/file must be called first");
    if (t_yaml_env_parse_data(env, 0, out) < 0) {
        res = -1;
        goto end;
    }

    RETHROW(yaml_env_ltrim(env));
    if (!ps_done(&env->ps)) {
        yaml_env_set_err(env, YAML_ERR_EXTRA_DATA,
                         "expected end of document");
        res = -1;
        goto end;
    }

    if (qm_len(yaml_vars, &env->variables) > 0
    &&  !(env->flags & YAML_PARSE_ALLOW_UNBOUND_VARIABLES))
    {
        set_unbound_variables_err(env);
        res = -1;
        goto end;
    }

  end:
    if (res < 0) {
        sb_setsb(out_err, &env->err);
    }
    /* reset the stream to the input, so that it can be properly returned
     * by yaml_parse_get_stream(). */
    env->ps = saved_ps;
    return res;
}

void t_yaml_data_get_presentation(
    const yaml_data_t * nonnull data,
    yaml__document_presentation__t * nonnull pres
)
{
    qv_t(pres_mapping) mappings;
    SB_1k(path);

    iop_init(yaml__document_presentation, pres);
    t_qv_init(&mappings, 0);
    t_yaml_add_pres_mappings(data, &path, &mappings);
    pres->mappings = IOP_TYPED_ARRAY_TAB(yaml__presentation_node_mapping,
                                         &mappings);
}

static const yaml_presentation_t * nonnull
t_yaml_doc_pres_to_map(const yaml__document_presentation__t *doc_pres)
{
    yaml_presentation_t *pres = t_new(yaml_presentation_t, 1);

    t_qm_init(yaml_pres_node, &pres->nodes, 0);
    tab_for_each_ptr(mapping, &doc_pres->mappings) {
        int res;

        res = qm_add(yaml_pres_node, &pres->nodes, &mapping->path,
                     &mapping->node);
        assert (res >= 0);
    }

    return pres;
}

void yaml_parse_pretty_print_err(const yaml_span_t * nonnull span,
                                 lstr_t error_msg, sb_t * nonnull out)
{
    pstream_t ps;
    bool one_liner;

    if (span->env->included) {
        yaml_parse_pretty_print_err(&span->env->included->data.span,
                                    LSTR("error in included file"), out);
        sb_addc(out, '\n');
    }

    if (span->env->filepath) {
        sb_addf(out, "%s:", span->env->filepath);
    } else {
        sb_adds(out, "<string>:");
    }
    sb_addf(out, YAML_POS_FMT": %pL", YAML_POS_ARG(span->start), &error_msg);

    one_liner = span->end.line_nb == span->start.line_nb;

    /* get the full line including pos_start */
    ps.s = span->start.s;
    ps.s -= span->start.col_nb - 1;

    /* find the end of the line */
    ps.s_end = one_liner ? span->end.s - 1 : ps.s;
    while (ps.s_end < span->env->ps.s_end && *ps.s_end != '\n') {
        ps.s_end++;
    }
    if (ps_len(&ps) == 0) {
        return;
    }

    /* print the whole line */
    sb_addf(out, "\n%*pM\n", PS_FMT_ARG(&ps));

    /* then display some indications or where the issue is */
    if (span->start.col_nb > 1) {
        sb_addnc(out, span->start.col_nb - 1, ' ');
    }
    if (one_liner) {
        assert (span->end.col_nb > span->start.col_nb);
        sb_addnc(out, span->end.col_nb - span->start.col_nb, '^');
    } else {
        sb_adds(out, "^ starting here");
    }
}

/* }}} */
/* {{{ Packer */
/* {{{ Packing types */
/* {{{ Variables */

/** Deduced value of a variable. */
typedef struct yaml_variable_value_t {
    /* If NULL, variable's value has not been deduced yet. */
    const yaml_data_t * nullable data;
} yaml_variable_value_t;

/* Mapping from variable name, to deduced value */
qm_kvec_t(active_vars, lstr_t, yaml_variable_value_t, qhash_lstr_hash,
          qhash_lstr_equal);

/* }}} */
/* }}} */

#define YAML_STD_INDENT  2

/* State describing the state of the packing "cursor".
 *
 * This is used to properly insert newlines & indentations between every key,
 * sequence, data, comments, etc.
 */
typedef enum yaml_pack_state_t {
    /* Clean state for writing. This state is required before writing any
     * new data. */
    PACK_STATE_CLEAN,

    /* On sequence dash, ie the "-" of a new sequence element. */
    PACK_STATE_ON_DASH,

    /* On object key, ie the ":" of a new object key. */
    PACK_STATE_ON_KEY,

    /* On a newline */
    PACK_STATE_ON_NEWLINE,

    /* After having wrote data. */
    PACK_STATE_AFTER_DATA,
} yaml_pack_state_t;

qm_kvec_t(path_to_checksum, lstr_t, uint64_t, qhash_lstr_hash,
          qhash_lstr_equal);

typedef struct yaml_pack_env_t {
    /* Write callback + priv data. */
    yaml_pack_writecb_f *write_cb;
    void *priv;

    /* Current packing state.
     *
     * Used to prettify the output by correctly transitioning between states.
     */
    yaml_pack_state_t state;

    /* Indent level (in number of spaces). */
    int indent_lvl;

    /* Presentation data, if provided. */
    const yaml_presentation_t * nullable pres;

    /* Path from the root document.
     *
     * When packing the root document, this is equivalent to the current path.
     * However, when packing a subfile, this includes the path from the
     * including document.
     * To get the current path only, see yaml_pack_env_get_curpath.
     *
     * Absolute paths are used for overrides through includes, see
     * yaml_pack_override_t::absolute_path.
     */
    sb_t absolute_path;

    /* Start of current path being packed.
     *
     * This is the index of the start of the current path in the
     * absolute_path buffer.
     */
    unsigned current_path_pos;

    /* Error buffer. */
    sb_t err;

    /* Path to the output directory. */
    lstr_t outdirpath;

    /* Flags to use when creating subfiles. */
    unsigned file_flags;

    /* Mode to use when creating subfiles. */
    mode_t file_mode;

    /* Bitfield of yaml_pack_flags_t elements. */
    unsigned flags;

    /* Packed subfiles.
     *
     * Associates paths to created subfiles with a checksum of the file's
     * content. This is used to handle shared subfiles.
     */
    qm_t(path_to_checksum) * nullable subfiles;

    /** Information about overridden values.
     *
     * This is a *stack* of currently active overrides. The last element
     * is the most recent override, and matching overridden values should thus
     * be done in reverse order.
     */
    qv_t(pack_override) overrides;

    /** Information about substituted variables.
     *
     * This is a *stack* of currently active variables (i.e., variable names
     * that are handled by an override). The last element is the most recent
     * override, and matching variable values should thus be done in reverse
     * order.
     */
    /* TODO: do the stack and test it */
    qm_t(active_vars) active_vars;
} yaml_pack_env_t;

static int t_yaml_pack_data(yaml_pack_env_t * nonnull env,
                            const yaml_data_t * nonnull data);

/* {{{ Utils */

static int do_write(yaml_pack_env_t *env, const void *_buf, int len)
{
    const uint8_t *buf = _buf;
    int pos = 0;

    while (pos < len) {
        int res = (*env->write_cb)(env->priv, buf + pos, len - pos,
                                   &env->err);

        if (res < 0) {
            if (ERR_RW_RETRIABLE(errno)) {
                continue;
            }
            return -1;
        }
        pos += res;
    }
    return len;
}

static int do_indent(yaml_pack_env_t *env)
{
    static lstr_t spaces = LSTR_IMMED("                                    ");
    int todo = env->indent_lvl;

    while (todo > 0) {
        int res = (*env->write_cb)(env->priv, spaces.s, MIN(spaces.len, todo),
                                   &env->err);

        if (res < 0) {
            if (ERR_RW_RETRIABLE(errno)) {
                continue;
            }
            return -1;
        }
        todo -= res;
    }

    env->state = PACK_STATE_CLEAN;

    return env->indent_lvl;
}

#define WRITE(data, len)                                                     \
    do {                                                                     \
        res += RETHROW(do_write(env, data, len));                            \
    } while (0)
#define PUTS(s)  WRITE(s, strlen(s))
#define PUTLSTR(s)  WRITE(s.data, s.len)

#define INDENT()                                                             \
    do {                                                                     \
        res += RETHROW(do_indent(env));                                      \
    } while (0)

#define GOTO_STATE(state)                                                    \
    do {                                                                     \
        res += RETHROW(yaml_pack_goto_state(env, PACK_STATE_##state));       \
    } while (0)

static int yaml_pack_goto_state(yaml_pack_env_t *env,
                                yaml_pack_state_t new_state)
{
    int res = 0;

    switch (env->state) {
      case PACK_STATE_CLEAN:
        switch (new_state) {
          case PACK_STATE_ON_NEWLINE:
            PUTS("\n");
            break;
          case PACK_STATE_AFTER_DATA:
          case PACK_STATE_CLEAN:
          case PACK_STATE_ON_DASH:
          case PACK_STATE_ON_KEY:
            break;
        };
        break;

      case PACK_STATE_ON_DASH:
        switch (new_state) {
          case PACK_STATE_CLEAN:
          case PACK_STATE_ON_KEY:
          case PACK_STATE_ON_DASH:
            /* a key or seq dash is put on the same line as the seq dash */
            PUTS(" ");
            break;
          case PACK_STATE_ON_NEWLINE:
            PUTS("\n");
            break;
          case PACK_STATE_AFTER_DATA:
            break;
        };
        break;

      case PACK_STATE_ON_KEY:
        switch (new_state) {
          case PACK_STATE_CLEAN:
            PUTS(" ");
            break;
          case PACK_STATE_ON_NEWLINE:
            PUTS("\n");
            break;
          case PACK_STATE_ON_DASH:
          case PACK_STATE_ON_KEY:
            /* a seq dash or a new key is put on a newline after the key */
            PUTS("\n");
            INDENT();
            break;
          case PACK_STATE_AFTER_DATA:
            break;
        };
        break;

      case PACK_STATE_ON_NEWLINE:
        switch (new_state) {
          case PACK_STATE_CLEAN:
          case PACK_STATE_ON_DASH:
          case PACK_STATE_ON_KEY:
            INDENT();
            break;
          case PACK_STATE_ON_NEWLINE:
          case PACK_STATE_AFTER_DATA:
            break;
        };
        break;

      case PACK_STATE_AFTER_DATA:
        switch (new_state) {
          case PACK_STATE_ON_NEWLINE:
            PUTS("\n");
            break;
          case PACK_STATE_CLEAN:
            PUTS(" ");
            break;
          case PACK_STATE_ON_DASH:
          case PACK_STATE_ON_KEY:
            PUTS("\n");
            INDENT();
            break;
          case PACK_STATE_AFTER_DATA:
            break;
        };
        break;
    }

    env->state = new_state;

    return res;
}

static int yaml_pack_tag(yaml_pack_env_t * nonnull env, const lstr_t tag)
{
    int res = 0;

    if (tag.s) {
        GOTO_STATE(CLEAN);
        PUTS("!");
        PUTLSTR(tag);
        env->state = PACK_STATE_AFTER_DATA;
    }

    return res;
}

static yaml_pack_override_node_t * nullable
yaml_pack_env_find_override(yaml_pack_env_t * nonnull env)
{
    t_scope;
    lstr_t abspath;

    if (env->overrides.len == 0) {
        return NULL;
    }

    abspath = LSTR_SB_V(&env->absolute_path);
    tab_for_each_pos_rev(ov_pos, &env->overrides) {
        yaml_pack_override_t *override = &env->overrides.tab[ov_pos];
        int qm_pos;

        qm_pos = qm_find(override_nodes, &override->nodes, &abspath);
        if (qm_pos >= 0) {
            return &override->nodes.values[qm_pos];
        }
    }

    return NULL;
}

/* }}} */
/* {{{ Presentation */

/* XXX: need a prototype declaration to specify the __attr_printf__ */
static int
yaml_pack_env_push_path(yaml_pack_env_t * nullable env, const char *fmt,
                        ...)
    __attr_printf__(2, 3);

static int
yaml_pack_env_push_path(yaml_pack_env_t * nullable env, const char *fmt,
                        ...)
{
    int prev_len;
    va_list args;

    if (!env->pres) {
        return 0;
    }

    prev_len = env->absolute_path.len;
    va_start(args, fmt);
    sb_addvf(&env->absolute_path, fmt, args);
    va_end(args);

    return prev_len;
}

static void
yaml_pack_env_pop_path(yaml_pack_env_t * nullable env, int prev_len)
{
    if (!env->pres) {
        return;
    }

    sb_clip(&env->absolute_path, prev_len);
}

static lstr_t
yaml_pack_env_get_curpath(const yaml_pack_env_t * nonnull env)
{
    return LSTR_PTR_V(env->absolute_path.data + env->current_path_pos,
                      env->absolute_path.data + env->absolute_path.len);
}

static const yaml__presentation_node__t * nullable
yaml_pack_env_get_pres_node(yaml_pack_env_t * nonnull env)
{
    lstr_t path = yaml_pack_env_get_curpath(env);

    assert (env->pres);
    return qm_get_def_safe(yaml_pres_node, &env->pres->nodes, &path, NULL);
}

static int
yaml_pack_empty_lines(yaml_pack_env_t * nonnull env, uint8_t nb_lines)
{
    int res = 0;

    if (nb_lines == 0) {
        return 0;
    }

    GOTO_STATE(ON_NEWLINE);
    for (uint8_t i = 0; i < nb_lines; i++) {
        PUTS("\n");
    }

    return res;
}

static int
yaml_pack_pres_node_prefix(yaml_pack_env_t * nonnull env,
                           const yaml__presentation_node__t * nullable node)
{
    int res = 0;

    if (!node) {
        return 0;
    }

    res += yaml_pack_empty_lines(env, node->empty_lines);

    if (node->prefix_comments.len == 0) {
        return 0;
    }
    GOTO_STATE(ON_NEWLINE);
    tab_for_each_entry(comment, &node->prefix_comments) {
        GOTO_STATE(CLEAN);

        PUTS("# ");
        PUTLSTR(comment);
        PUTS("\n");
        env->state = PACK_STATE_ON_NEWLINE;
    }

    return res;
}

static int
yaml_pack_pres_node_inline(yaml_pack_env_t * nonnull env,
                           const yaml__presentation_node__t * nullable node)
{
    int res = 0;

    if (node && node->inline_comment.len > 0) {
        GOTO_STATE(CLEAN);
        PUTS("# ");
        PUTLSTR(node->inline_comment);
        PUTS("\n");
        env->state = PACK_STATE_ON_NEWLINE;
    }

    return res;
}

/* }}} */
/* {{{ Pack scalar */

/* ints:   sign, 20 digits, and NUL -> 22
 * double: sign, digit, dot, 17 digits, e, sign, up to 3 digits NUL -> 25
 */
#define IBUF_LEN  25

static bool yaml_string_must_be_quoted(const lstr_t s)
{
    /* '!', '&', '*', '-', '"' and '.' have special YAML meaning.
     * Technically, '-' is only forbidden if followed by a space,
     * but it is simpler that way.
     * Also forbid starting with '[' or '{'. In YAML, this indicates inline
     * JSON, which we do not handle in our parser, but would render the YAML
     * invalid for other parsers.
     */
    static ctype_desc_t const yaml_invalid_raw_string_start = { {
        0x00000000, 0x00006446, 0x08000000, 0x08000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    } };
    /* printable ascii characters minus ':' and '#'. Also should be
     * followed by space to be forbidden, but simpler that way. */
    static ctype_desc_t const yaml_raw_string_contains = { {
        0x00000000, 0xfbfffff7, 0xffffffff, 0xffffffff,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    } };

    if (s.len == 0) {
        return true;
    }

    /* cannot start with those characters */
    if (ctype_desc_contains(&yaml_invalid_raw_string_start, s.s[0])) {
        return true;
    }
    /* cannot contain those characters */
    if (!lstr_match_ctype(s, &yaml_raw_string_contains)) {
        return true;
    }
    /* cannot start or end with a space */
    if (lstr_startswith(s, LSTR(" ")) || lstr_endswith(s, LSTR(" "))) {
        return true;
    }
    if (lstr_equal(s, LSTR("~")) || lstr_equal(s, LSTR("null"))) {
        return true;
    }

    return false;
}

static int yaml_pack_string(yaml_pack_env_t *env, lstr_t val)
{
    int res = 0;
    pstream_t ps;

    if (!yaml_string_must_be_quoted(val)) {
        PUTLSTR(val);
        return res;
    }

    ps = ps_initlstr(&val);
    PUTS("\"");
    while (!ps_done(&ps)) {
        /* r:32-127 -s:'\\"' */
        static ctype_desc_t const safe_chars = { {
            0x00000000, 0xfffffffb, 0xefffffff, 0xffffffff,
                0x00000000, 0x00000000, 0x00000000, 0x00000000,
        } };
        const uint8_t *p = ps.b;
        size_t nbchars;
        int c;

        nbchars = ps_skip_span(&ps, &safe_chars);
        WRITE(p, nbchars);

        if (ps_done(&ps)) {
            break;
        }

        /* Assume broken utf-8 is mixed latin1 */
        c = ps_getuc(&ps);
        if (unlikely(c < 0)) {
            c = ps_getc(&ps);
        }
        switch (c) {
          case '"':  PUTS("\\\""); break;
          case '\\': PUTS("\\\\"); break;
          case '\a': PUTS("\\a"); break;
          case '\b': PUTS("\\b"); break;
          case '\e': PUTS("\\e"); break;
          case '\f': PUTS("\\f"); break;
          case '\n': PUTS("\\n"); break;
          case '\r': PUTS("\\r"); break;
          case '\t': PUTS("\\t"); break;
          case '\v': PUTS("\\v"); break;
          default: {
            char ibuf[IBUF_LEN];

            WRITE(ibuf, sprintf(ibuf, "\\u%04x", c));
          } break;
        }
    }
    PUTS("\"");

    return res;
}

static int yaml_pack_scalar(yaml_pack_env_t * nonnull env,
                            const yaml_scalar_t * nonnull scalar,
                            const lstr_t tag)
{
    int res = 0;
    char ibuf[IBUF_LEN];

    GOTO_STATE(CLEAN);

    switch (scalar->type) {
      case YAML_SCALAR_STRING:
        res += yaml_pack_string(env, scalar->s);
        break;

      case YAML_SCALAR_DOUBLE: {
        int inf = isinf(scalar->d);

        if (inf == 1) {
            PUTS(".Inf");
        } else
        if (inf == -1) {
            PUTS("-.Inf");
        } else
        if (isnan(scalar->d)) {
            PUTS(".NaN");
        } else {
            WRITE(ibuf, sprintf(ibuf, "%g", scalar->d));
        }
      } break;

      case YAML_SCALAR_UINT:
        WRITE(ibuf, sprintf(ibuf, "%ju", scalar->u));
        break;

      case YAML_SCALAR_INT:
        WRITE(ibuf, sprintf(ibuf, "%jd", scalar->i));
        break;

      case YAML_SCALAR_BOOL:
        if (scalar->b) {
            PUTS("true");
        } else {
            PUTS("false");
        }
        break;

      case YAML_SCALAR_NULL:
        PUTS("~");
        break;
    }

    env->state = PACK_STATE_AFTER_DATA;

    return res;
}

/* }}} */
/* {{{ Pack sequence */

static int t_yaml_pack_seq(yaml_pack_env_t * nonnull env,
                           const yaml_seq_t * nonnull seq)
{
    int res = 0;

    if (seq->datas.len == 0) {
        GOTO_STATE(CLEAN);
        PUTS("[]");
        env->state = PACK_STATE_AFTER_DATA;
        return res;
    }

    tab_for_each_pos(pos, &seq->datas) {
        const yaml__presentation_node__t *node = NULL;
        const yaml_data_t *data = &seq->datas.tab[pos];
        yaml_pack_override_node_t *override = NULL;
        int path_len = 0;

        if (env->pres) {
            path_len = yaml_pack_env_push_path(env, "[%d]", pos);
            node = yaml_pack_env_get_pres_node(env);
        } else
        if (pos < seq->pres_nodes.len && seq->pres_nodes.tab[pos]) {
            node = seq->pres_nodes.tab[pos];
        }

        override = yaml_pack_env_find_override(env);
        if (override && likely(!override->data)) {
            /* The node was added by an override. Save it in the override
             * data, and ignore the node. */
            logger_trace(&_G.logger, 2,
                         "not packing overridden data in path `%*pM`",
                         LSTR_FMT_ARG(yaml_pack_env_get_curpath(env)));
            override->data = data;
            override->found = true;
            goto next;
        }

        res += yaml_pack_pres_node_prefix(env, node);

        GOTO_STATE(ON_DASH);
        PUTS("-");

        env->indent_lvl += YAML_STD_INDENT;
        res += yaml_pack_pres_node_inline(env, node);
        res += RETHROW(t_yaml_pack_data(env, data));
        env->indent_lvl -= YAML_STD_INDENT;

      next:
        yaml_pack_env_pop_path(env, path_len);
    }

    return res;
}

/* }}} */
/* {{{ Pack object */

static int t_yaml_pack_key_data(yaml_pack_env_t * nonnull env,
                                const yaml_key_data_t * nonnull kd)
{
    int res = 0;
    int path_len = 0;
    const yaml__presentation_node__t *node;
    yaml_pack_override_node_t *override = NULL;

    if (env->pres) {
        path_len = yaml_pack_env_push_path(env, ".%pL", &kd->key);
        node = yaml_pack_env_get_pres_node(env);
    } else {
        node = kd->key_presentation;
    }

    override = yaml_pack_env_find_override(env);
    if (override && likely(!override->data)) {
        /* The node was added by an override. Save it in the override
         * data, and ignore the node. */
        logger_trace(&_G.logger, 2, "not packing overridden data in path "
                     "`%*pM`", LSTR_FMT_ARG(yaml_pack_env_get_curpath(env)));
        override->data = &kd->data;
        override->found = true;
        goto end;
    }

    res += yaml_pack_pres_node_prefix(env, node);

    GOTO_STATE(ON_KEY);
    PUTLSTR(kd->key);
    PUTS(":");

    /* for scalars, we put the inline comment after the value:
     *  key: val # comment
     */
    env->indent_lvl += YAML_STD_INDENT;
    res += yaml_pack_pres_node_inline(env, node);
    res += RETHROW(t_yaml_pack_data(env, &kd->data));
    env->indent_lvl -= YAML_STD_INDENT;

  end:
    yaml_pack_env_pop_path(env, path_len);

    return res;
}

static int yaml_pack_obj(yaml_pack_env_t * nonnull env,
                         const yaml_obj_t * nonnull obj)
{
    int res = 0;

    if (obj->fields.len == 0) {
        GOTO_STATE(CLEAN);
        PUTS("{}");
        env->state = PACK_STATE_AFTER_DATA;
    } else {
        tab_for_each_ptr(pair, &obj->fields) {
            res += RETHROW(t_yaml_pack_key_data(env, pair));
        }
    }

    return res;
}

/* }}} */
/* {{{ Pack flow */

static int yaml_pack_flow_data(yaml_pack_env_t * nonnull env,
                               const yaml_data_t * nonnull data,
                               bool can_omit_brackets);

static int yaml_pack_flow_seq(yaml_pack_env_t * nonnull env,
                              const yaml_seq_t * nonnull seq)
{
    int res = 0;

    if (seq->datas.len == 0) {
        PUTS("[]");
        return res;
    }

    PUTS("[ ");
    tab_for_each_pos(pos, &seq->datas) {
        const yaml_data_t *data = &seq->datas.tab[pos];

        if (pos > 0) {
            PUTS(", ");
        }
        res += RETHROW(yaml_pack_flow_data(env, data, true));
    }
    PUTS(" ]");

    return res;
}

/* can_omit_brackets is used to prevent the packing of a single key object
 * inside an object:
 *   a: b: v
 * which is not valid.
 */
static int yaml_pack_flow_obj(yaml_pack_env_t * nonnull env,
                              const yaml_obj_t * nonnull obj,
                              bool can_omit_brackets)
{
    int res = 0;
    bool omit_brackets;

    if (obj->fields.len == 0) {
        PUTS("{}");
        return res;
    }

    omit_brackets = can_omit_brackets && obj->fields.len == 1;
    if (!omit_brackets) {
        PUTS("{ ");
    }
    tab_for_each_pos(pos, &obj->fields) {
        const yaml_key_data_t *kd = &obj->fields.tab[pos];

        if (pos > 0) {
            PUTS(", ");
        }
        PUTLSTR(kd->key);
        PUTS(": ");
        res += RETHROW(yaml_pack_flow_data(env, &kd->data, false));
    }
    if (!omit_brackets) {
        PUTS(" }");
    }

    return res;
}

static int yaml_pack_flow_data(yaml_pack_env_t * nonnull env,
                               const yaml_data_t * nonnull data,
                               bool can_omit_brackets)
{
    int res = 0;

    /* This is guaranteed by the yaml_data_can_use_flow_mode check. */
    assert (!data->tag.s);

    switch (data->type) {
      case YAML_DATA_SCALAR:
        res += RETHROW(yaml_pack_scalar(env, &data->scalar, LSTR_NULL_V));
        break;
      case YAML_DATA_SEQ:
        res += RETHROW(yaml_pack_flow_seq(env, data->seq));
        break;
      case YAML_DATA_OBJ:
        res += RETHROW(yaml_pack_flow_obj(env, data->obj, can_omit_brackets));
        break;
    }
    env->state = PACK_STATE_CLEAN;

    return res;
}

/* }}} */
/* {{{ Flow packable helpers */

static bool
yaml_env_path_contains_overrides(const yaml_pack_env_t * nonnull env)
{
    t_scope;
    lstr_t abspath;

    abspath = LSTR_SB_V(&env->absolute_path);
    tab_for_each_ptr(override, &env->overrides) {
        qm_for_each_key(override_nodes, key, &override->nodes) {
            if (lstr_startswith(key, abspath)) {
                return true;
            }
        }
    }

    return false;
}

static bool yaml_data_contains_tags(const yaml_data_t * nonnull data)
{
    if (data->tag.s) {
        return true;
    }

    switch (data->type) {
      case YAML_DATA_SCALAR:
        break;
      case YAML_DATA_SEQ:
        tab_for_each_ptr(elem, &data->seq->datas) {
            if (yaml_data_contains_tags(elem)) {
                return true;
            }
        }
        break;
      case YAML_DATA_OBJ:
        tab_for_each_ptr(kd, &data->obj->fields) {
            if (yaml_data_contains_tags(&kd->data)) {
                return true;
            }
        }
        break;
    }

    return false;
}

/** Make sure the data can be packed using flow mode.
 *
 * Flow mode is incompatible with the use of tags. If any data inside the
 * provided data has tags, flow mode cannot be used.
 */
static bool
yaml_env_data_can_use_flow_mode(const yaml_pack_env_t * nonnull env,
                                const yaml_data_t * nonnull data)
{
    /* If the flow data contains overrides, it cannot be packed into flow
     * mode. This isn't a hard limitation, but not implemented for the moment
     * because:
     *  * the use case seems limited
     *  * this would complicate the flow packing a lot, as it does not handle
     *    presentation.
     */
    if (yaml_env_path_contains_overrides(env)) {
        return false;
    }

    /* Recursing through the data to find out if it can be packed in a certain
     * way isn't ideal... This is acceptable here because flow data are
     * usually very small, and in the worst case, the whole data is in flow
     * mode, so we only go through the data twice.
     */
    if (yaml_data_contains_tags(data)) {
        return false;
    }

    return true;
}

/* }}} */
/* {{{ Pack override */

static void t_iop_pres_override_to_pack_override(
    const yaml_pack_env_t * nonnull env,
    const yaml__presentation_override__t * nonnull pres,
    yaml_pack_override_t *out)
{
    p_clear(out, 1);

    out->presentation = pres;
    t_qm_init(override_nodes, &out->nodes, pres->nodes.len);
    t_qv_init(&out->ordered_paths, pres->nodes.len);

    tab_for_each_ptr(node, &pres->nodes) {
        yaml_pack_override_node_t pack_node;
        yaml_data_t *data = NULL;
        lstr_t path;
        int res;

        if (node->original_data) {
            data = t_new(yaml_data_t, 1);
            t_iop_data_to_yaml(node->original_data, data);
        }
        p_clear(&pack_node, 1);
        pack_node.data = data;

        path = t_lstr_fmt("%pL%pL", &env->absolute_path, &node->path);
        res = qm_add(override_nodes, &out->nodes, &path, pack_node);
        assert (res >= 0);

        qv_append(&out->ordered_paths, path);
    }
}

static void
t_set_data_from_path(const yaml_data_t * nonnull data, pstream_t path,
                     bool new, yaml_data_t * nonnull out)
{
    if (ps_peekc(path) == '!' || ps_len(&path) == 0) {
        /* The ps_len == 0 can happen for added datas. The path ends with
         * [%d] or .%s, to mark the seq elem/obj key as the node being added.
         */
        *out = *data;
    } else
    if (ps_peekc(path) == '[') {
        yaml_data_t new_data;

        ps_skipc(&path, '.');
        /* We do not care about the index, because it is relative to the
         * overridden AST, not relevant here. Here, we only want to create
         * a sequence holding all our elements. */
        ps_skip_afterchr(&path, ']');

        if (new) {
            t_yaml_data_new_seq(out, 1);
        } else {
            /* This assert should not fail unless the presentation data was
             * malignly crafted. As it is created from a parsed AST, there
             * cannot be mixed data types through common path.
             * If this assert fails, it means the override contains paths
             * such as:
             *
             * .foo.bar
             * .foo[0]
             *
             * ie .foo is both an object and a sequence.
             */
            if (!expect(out->type == YAML_DATA_SEQ)) {
                return;
            }
        }

        t_set_data_from_path(data, path, true, &new_data);
        yaml_seq_add_data(out, new_data);
    } else
    if (ps_peekc(path) == '.') {
        yaml_data_t new_data;
        pstream_t ps_key;
        lstr_t key;

        ps_skipc(&path, '.');
        ps_key = ps_get_span(&path, &ctype_isalnum);
        key = LSTR_PS_V(&ps_key);

        if (new) {
            t_yaml_data_new_obj(out, 1);
        } else {
            /* see related expect in the seq case */
            if (!expect(out->type == YAML_DATA_OBJ)) {
                return;
            }

            tab_for_each_ptr(kd, &out->obj->fields) {
                if (lstr_equal(kd->key, key)) {
                    t_set_data_from_path(data, path, false, &kd->data);
                    return;
                }
            }
        }

        t_set_data_from_path(data, path, true, &new_data);
        yaml_obj_add_field(out, key, new_data);
    }
}

static int
t_build_override_data(const yaml_pack_override_t * nonnull override,
                      yaml_data_t * nonnull out)
{
    bool first = true;

    /* Iterate on the ordered paths, to make sure the data is recreated
     * in the right order. */
    assert (override->ordered_paths.len == override->presentation->nodes.len);
    tab_for_each_pos(pos, &override->ordered_paths) {
        const yaml_pack_override_node_t *node;
        pstream_t ps;

        node = qm_get_p_safe(override_nodes, &override->nodes,
                             &override->ordered_paths.tab[pos]);

        if (unlikely(!node->found)) {
            /* This can happen if an overrided node is no longer present
             * in the AST. In that case, ignore it.  */
            continue;
        }
        assert (node->data);

        /* Use the relative path here, to properly reconstruct the data. */
        ps = ps_initlstr(&override->presentation->nodes.tab[pos].path);
        t_set_data_from_path(node->data, ps, first, out);
        first = false;
    }

    /* if first is still true, the override is empty and should be ignored. */
    return first ? -1 : 0;
}

static int
t_yaml_pack_override(yaml_pack_env_t * nonnull env,
                     const yaml_pack_override_t * nonnull override)
{
    int res = 0;
    yaml_data_t data;
    const yaml_presentation_t *pres;
    unsigned current_path_pos;

    /* rebuild a yaml data from the override nodes */
    if (t_build_override_data(override, &data) < 0) {
        return 0;
    }

    pres = t_yaml_doc_pres_to_map(&override->presentation->presentation);
    current_path_pos = env->absolute_path.len;

    /* Pack the data in the output. To reuse the right presentation, it must
     * be set in the env, and the path reset so that it matches the
     * presentation.
     */
    /* TODO: Maybe create a new env? This is a bit of a mess. */

    SWAP(const yaml_presentation_t *, pres, env->pres);
    SWAP(unsigned, current_path_pos, env->current_path_pos);

    res = t_yaml_pack_data(env, &data);

    SWAP(unsigned, current_path_pos, env->current_path_pos);
    SWAP(const yaml_presentation_t *, pres, env->pres);

    return res;
}

/* }}} */
/* {{{ Pack include */
/* {{{ Subfile sharing handling */

enum subfile_status_t {
    SUBFILE_TO_CREATE,
    SUBFILE_TO_REUSE,
    SUBFILE_TO_IGNORE,
};

/* check if data can be packed in the subfile given from its relative path
 * from the env outdir */
static enum subfile_status_t
check_subfile(yaml_pack_env_t * nonnull env, uint64_t checksum,
              const char * nonnull relative_path)
{
    char fullpath[PATH_MAX];
    lstr_t path;
    int pos;

    /* compute new outdir */
    path_extend(fullpath, env->outdirpath.s, "%s", relative_path);
    path = LSTR(fullpath);

    assert (env->subfiles);
    pos = qm_put(path_to_checksum, env->subfiles, &path, checksum, 0);
    if (pos & QHASH_COLLISION) {
        pos &= ~QHASH_COLLISION;
        if (env->subfiles->values[pos] == checksum) {
            return SUBFILE_TO_REUSE;
        } else {
            return SUBFILE_TO_IGNORE;
        }
    } else {
        env->subfiles->keys[pos] = t_lstr_dup(path);
        return SUBFILE_TO_CREATE;
    }
}

static const char * nullable
t_find_right_path(yaml_pack_env_t * nonnull env, sb_t * nonnull contents,
                  lstr_t initial_path, bool * nonnull reuse)
{
    const char *ext;
    char *path;
    lstr_t base;
    int counter = 1;
    uint64_t checksum;

    /* TODO: it would be more efficient to compute the checksum as the
     * contents buffer is filled. */
    /* TODO: use full 256bits hash to prevent collision? */
    checksum = sha2_hash_64(contents->data, contents->len);

    path = t_fmt("%pL", &initial_path);
    path_simplify(path);

    ext = path_ext(path);
    base = ext ? LSTR_PTR_V(path, ext) : LSTR(path);

    /* check base.ext, base~1.ext, etc until either the file does not exist,
     * or the data to pack is identical to the data packed in the subfile. */
    for (;;) {
        switch (check_subfile(env, checksum, path)) {
          case SUBFILE_TO_CREATE:
            *reuse = false;
            return path;

          case SUBFILE_TO_REUSE:
            logger_trace(&_G.logger, 2, "subfile `%s` reused", path);
            *reuse = true;
            return path;

          case SUBFILE_TO_IGNORE:
            logger_trace(&_G.logger, 2,
                         "should have reused subfile `%s`, but the packed "
                         "data is different", path);
            break;
        }

        if (ext) {
            path = t_fmt("%pL~%d%s", &base, counter++, ext);
        } else {
            path = t_fmt("%pL~%d", &base, counter++);
        }
    }
}

/* }}} */
/* {{{ Include node packing */

/* Pack the "!include(raw)? <path>" node, with the right presentation. */
static int
t_yaml_pack_include_path(yaml_pack_env_t * nonnull env,
                         const yaml__presentation_node__t * nonnull pres,
                         bool raw, lstr_t include_path)
{
    const yaml_presentation_t *saved_pres = env->pres;
    yaml_data_t data;
    int res;

    yaml_data_set_string(&data, include_path);
    data.tag = raw ? LSTR("includeraw") : LSTR("include");
    data.presentation = unconst_cast(yaml__presentation_node__t, pres);

    /* Make sure the presentation data is not used as the paths won't be
     * correct when packing this data. */
    env->pres = NULL;
    res = t_yaml_pack_data(env, &data);
    env->pres = saved_pres;

    return res;
}

/* write raw contents directly into the given filepath. */
static int
yaml_pack_write_raw_file(const yaml_pack_env_t * nonnull env,
                         const char * nonnull filepath,
                         const lstr_t contents, sb_t * nonnull err)
{
    t_scope;
    const char *fullpath;
    char fulldirpath[PATH_MAX];
    file_t *file;

    fullpath = t_fmt("%pL/%s", &env->outdirpath, filepath);

    path_dirname(fulldirpath, PATH_MAX, fullpath);
    if (mkdir_p(fulldirpath, 0755) < 0) {
        sb_sets(err, "could not create output directory: %m");
        return -1;
    }

    file = file_open(fullpath, env->file_flags, env->file_mode);
    if (!file) {
        sb_setf(err, "cannot open output file `%s`: %m", fullpath);
        return -1;
    }

    if (file_write(file, contents.s, contents.len) < 0) {
        sb_setf(err, "cannot write in output file: %m");
        return -1;
    }

    IGNORE(file_close(&file));
    return 0;
}

/* }}} */
/* {{{ Pack subfile */

static int write_nothing(void *b, const void *buf, int len, sb_t *err)
{
    return len;
}

static int
t_yaml_pack_subfile_in_sb(yaml_pack_env_t * nonnull env,
                          const yaml__presentation_include__t * nonnull inc,
                          const yaml_data_t * nonnull data, bool no_subfiles,
                          sb_t * nonnull out, sb_t * nonnull err)
{
    yaml_pack_env_t *subenv = t_yaml_pack_env_new();

    if (!no_subfiles) {
        const char *fullpath;
        char dirpath[PATH_MAX];

        fullpath = t_fmt("%pL/%pL", &env->outdirpath, &inc->path);
        path_dirname(dirpath, PATH_MAX, fullpath);

        RETHROW(t_yaml_pack_env_set_outdir(subenv, dirpath, err));
    }

    t_yaml_pack_env_set_presentation(subenv, &inc->document_presentation);

    sb_setsb(&subenv->absolute_path, &env->absolute_path);
    subenv->current_path_pos = subenv->absolute_path.len;
    yaml_pack_env_set_flags(subenv, env->flags);

    subenv->overrides = env->overrides;
    subenv->active_vars = env->active_vars;

    /* Make sure the subfiles qm is shared, so that if this subfile
     * also generate other subfiles, it is properly handled. */
    subenv->subfiles = env->subfiles;

    if (no_subfiles) {
        /* Go through the AST as if the file was packed, but do not actually
         * write anything. This allows properly recreating the overrides. */
        RETHROW(t_yaml_pack(subenv, data, &write_nothing, NULL, err));
    } else {
        RETHROW(t_yaml_pack_sb(subenv, data, out, err));

        /* always ends with a newline when packing for a file */
        if (out->len > 0 && out->data[out->len - 1] != '\n') {
            sb_addc(out, '\n');
        }
    }

    return 0;
}

static int
t_yaml_pack_included_subfile(
    yaml_pack_env_t * nonnull env,
    const yaml__presentation_include__t * nonnull inc,
    const yaml_data_t * nonnull subdata)
{
    const char *path;
    bool reuse;
    bool raw = inc->raw;
    bool no_subfiles = env->flags & YAML_PACK_NO_SUBFILES;
    int res = 0;
    SB_1k(contents);
    SB_1k(err);

    if (!env->subfiles) {
        env->subfiles = t_qm_new(path_to_checksum, 0);
    }

    /* if the YAML data to dump is not a string, it changed and can no longer
     * be packed raw. */
    if (raw && (subdata->type != YAML_DATA_SCALAR
            ||  subdata->scalar.type != YAML_SCALAR_STRING))
    {
        raw = false;
    }

    if (raw) {
        sb_set_lstr(&contents, subdata->scalar.s);
    } else {
        /* Pack the subdata, but in a sb, not in the subfile directly. As the
         * subfile can be shared by multiple includes, we need to ensure
         * the contents are the same to share the same filename, or use
         * another one.
         *
         * Additionally, as packing the subfiles might have side-effects on
         * the current env (mainly, overrides packed in this env depends on
         * the packing of the subdata), it is not possible to do some sort
         * of AST comparison to detect shared subfiles.
         */
        if (t_yaml_pack_subfile_in_sb(env, inc, subdata, no_subfiles,
                                      &contents, &err) < 0)
        {
            sb_setf(&env->err, "cannot pack subfile `%pL`: %pL", &inc->path,
                    &err);
            return -1;
        }
    }

    path = t_find_right_path(env, &contents, inc->path, &reuse);
    if (reuse) {
        res += RETHROW(t_yaml_pack_include_path(env,
                                                inc->include_presentation,
                                                raw, LSTR(path)));
    } else {
        logger_trace(&_G.logger, 2, "writing %ssubfile %s", raw ? "raw " : "",
                     path);
        if (likely(!no_subfiles)
        &&  yaml_pack_write_raw_file(env, path, LSTR_SB_V(&contents),
                                     &err) < 0)
        {
            sb_setf(&env->err, "error when writing subfile `%s`: %pL",
                    path, &err);
            return -1;
        }

        res += RETHROW(t_yaml_pack_include_path(env,
                                                inc->include_presentation,
                                                raw, LSTR(path)));
    }

    return res;
}

static int
t_yaml_pack_variable_settings(yaml_pack_env_t * nonnull env)
{
    yaml_data_t data;
    const yaml_presentation_t *pres = NULL;
    int res;

    t_yaml_data_new_obj(&data, qm_len(active_vars, &env->active_vars));
    qm_for_each_key_value_p(active_vars, name, var, &env->active_vars) {
        lstr_t var_name = t_lstr_fmt("$%pL", &name);

        if (var->data) {
            yaml_obj_add_field(&data, var_name, *var->data);
        }
    }
    qm_clear(active_vars, &env->active_vars);

    if (data.obj->fields.len == 0) {
        return 0;
    }

    SWAP(const yaml_presentation_t *, pres, env->pres);
    res = t_yaml_pack_data(env, &data);
    SWAP(const yaml_presentation_t *, pres, env->pres);

    return res;
}

static int
t_yaml_pack_include_with_override(
    yaml_pack_env_t * nonnull env,
    yaml__presentation_include__t * nonnull inc,
    const yaml_data_t * nonnull subdata)
{
    yaml_pack_override_t *override = NULL;
    int res = 0;

    /* add current override if it exists, so that it is used when the subdata
     * is packed. */
    if (inc->override) {
        override = qv_growlen0(&env->overrides, 1);
        t_iop_pres_override_to_pack_override(env, inc->override,
                                             override);
    }
    if (inc->variables) {
        tab_for_each_entry(name, &inc->variables->names) {
            yaml_variable_value_t var = {0};

            /* TODO: handle multiple overrides */
            qm_add(active_vars, &env->active_vars, &name, var);
        }
    }

    res += RETHROW(t_yaml_pack_included_subfile(env, inc, subdata));

    if (inc->variables) {
        res += RETHROW(t_yaml_pack_variable_settings(env));
    }

    if (override) {
        qv_remove_last(&env->overrides);
        logger_trace(&_G.logger, 2, "packing override %pL", &inc->path);
        res += RETHROW(t_yaml_pack_override(env, override));
    }

    return res;
}

/* }}} */

static int
t_yaml_pack_included_data(yaml_pack_env_t * nonnull env,
                          const yaml_data_t * nonnull data,
                          const yaml__presentation_node__t * nonnull node)
{
    yaml__presentation_include__t *inc;

    inc = node->included;
    /* Write include node & override if:
     *  * an outdir is set
     *  * NO_SUBFILES is set, meaning we are recreating the file as is.
     */
    if (env->outdirpath.len > 0 || env->flags & YAML_PACK_NO_SUBFILES) {
        return t_yaml_pack_include_with_override(env, inc, data);
    } else {
        const yaml_presentation_t *saved_pres = env->pres;
        unsigned current_path_pos = env->absolute_path.len;
        int res;

        /* Inline the contents of the included data directly in the current
         * stream. This is as easy as just packing data, but we need to also
         * use the presentation data from the included files. To do so, the
         * current_path must be reset. */
        SWAP(unsigned, current_path_pos, env->current_path_pos);
        env->pres = t_yaml_doc_pres_to_map(&inc->document_presentation);

        res = t_yaml_pack_data(env, data);

        env->pres = saved_pres;
        SWAP(unsigned, current_path_pos, env->current_path_pos);

        return res;
    }
}

/* }}} */
/* {{{ Variables */

static int
t_apply_variable_value(yaml_pack_env_t * nonnull env, lstr_t var_name,
                       const yaml_data_t * nonnull data)
{
    yaml_variable_value_t *var;

    var = qm_get_def_p(active_vars, &env->active_vars, &var_name, NULL);
    if (!var) {
        return -1;
    }

    logger_trace(&_G.logger, 2, "deduced value for variable `%pL` to %s",
                 &var_name, yaml_data_get_type(data, false));

    if (var) {
        if (var->data) {
            /* TODO: handle collisions */
        }
        var->data = data;
    }

    return 0;
}

/* Deduce values for active variables, by comparing the original string
 * containing variables, with the value of the AST.
 *
 * For example:
 *   var_string = "$name_$a"
 *   data = string: "toto_t"
 *
 * => name = toto
 *    a = t
 *
 * If all variables cannot be deduced, -1 is returned, and the original string
 * with variables is not used.
 */
static int
t_deduce_variable_values(yaml_pack_env_t * nonnull env, lstr_t var_string,
                         const yaml_data_t * nonnull data)
{
    pstream_t ps = ps_initlstr(&var_string);
    pstream_t name;

    /* TODO: handle more cases than just "$name" */
    if (ps_skipc(&ps, '$') < 0) {
        return -1;
    }
    name = ps_get_span(&ps, &ctype_isalnum);
    if (ps_len(&name) <= 0 || !ps_done(&ps)) {
        return -1;
    }

    return t_apply_variable_value(env, LSTR_PS_V(&name), data);
}

/* }}} */
/* {{{ Pack data */

static int t_yaml_pack_data(yaml_pack_env_t * nonnull env,
                            const yaml_data_t * nonnull data)
{
    const yaml__presentation_node__t *node;
    yaml_pack_override_node_t *override = NULL;
    int res = 0;

    if (env->pres) {
        int path_len = yaml_pack_env_push_path(env, "!");

        node = yaml_pack_env_get_pres_node(env);
        override = yaml_pack_env_find_override(env);
        /* This should only be a replace, as additions can only be done
         * on keys or seq indicators. So *override should be not NULL, but
         * as a user can write its own presentation data, we cannot assert
         * it. */
        if (override && likely(override->data)) {
            logger_trace(&_G.logger, 2,
                         "packing non-overriden data in path `%*pM`",
                         LSTR_FMT_ARG(yaml_pack_env_get_curpath(env)));
            SWAP(const yaml_data_t *, data, override->data);
            override->found = true;
        }
        yaml_pack_env_pop_path(env, path_len);
    } else {
        node = data->presentation;
    }

    /* If the node was included from another file, and we are packing files,
     * dump it in a new file. */
    if (unlikely(node && node->included)) {
        return t_yaml_pack_included_data(env, data, node);
    }

    if (node) {
        if (node->value_with_variables.s
        &&  t_deduce_variable_values(env, node->value_with_variables,
                                     data) >= 0)
        {
            yaml_data_t *new_data = t_new(yaml_data_t, 1);

            yaml_data_set_string(new_data, node->value_with_variables);
            data = new_data;
        }
        res += yaml_pack_pres_node_prefix(env, node);
    }

    res += yaml_pack_tag(env, data->tag);

    if (node && node->flow_mode && yaml_env_data_can_use_flow_mode(env, data))
    {
        GOTO_STATE(CLEAN);
        res += yaml_pack_flow_data(env, data, false);
        env->state = PACK_STATE_AFTER_DATA;
    } else {
        switch (data->type) {
          case YAML_DATA_SCALAR: {
            res += RETHROW(yaml_pack_scalar(env, &data->scalar, data->tag));
          } break;
          case YAML_DATA_SEQ:
            res += RETHROW(t_yaml_pack_seq(env, data->seq));
            break;
          case YAML_DATA_OBJ:
            res += RETHROW(yaml_pack_obj(env, data->obj));
            break;
        }
    }

    if (node) {
        res += yaml_pack_pres_node_inline(env, node);
    }

    return res;
}

#undef WRITE
#undef PUTS
#undef PUTLSTR
#undef INDENT

/* }}} */
/* }}} */
/* {{{ Pack env public API */

/** Initialize a new YAML packing context. */
yaml_pack_env_t * nonnull t_yaml_pack_env_new(void)
{
    yaml_pack_env_t *env = t_new(yaml_pack_env_t, 1);

    env->state = PACK_STATE_ON_NEWLINE;
    env->file_flags = FILE_WRONLY | FILE_CREATE | FILE_TRUNC;
    env->file_mode = 0644;
    env->outdirpath = LSTR_EMPTY_V;

    t_sb_init(&env->absolute_path, 1024);
    t_sb_init(&env->err, 1024);
    t_qv_init(&env->overrides, 0);
    t_qm_init(active_vars, &env->active_vars, 0);

    return env;
}

void yaml_pack_env_set_flags(yaml_pack_env_t * nonnull env, unsigned flags)
{
    env->flags = flags;
}

int t_yaml_pack_env_set_outdir(yaml_pack_env_t * nonnull env,
                               const char * nonnull dirpath,
                               sb_t * nonnull err)
{
    char canonical_path[PATH_MAX];

    if (mkdir_p(dirpath, 0755) < 0) {
        sb_sets(err, "could not create output directory: %m");
        return -1;
    }

    /* Should not fail because any errors should have been caught by
     * mkdir_p first. */
    if (!expect(path_canonify(canonical_path, PATH_MAX, dirpath) >= 0)) {
        sb_setf(err, "cannot compute path to output directory `%s`: %m",
                dirpath);
        return -1;
    }

    env->outdirpath = t_lstr_dup(LSTR(canonical_path));

    return 0;
}

void yaml_pack_env_set_file_mode(yaml_pack_env_t * nonnull env, mode_t mode)
{
    env->file_mode = mode;
}

void t_yaml_pack_env_set_presentation(
    yaml_pack_env_t * nonnull env,
    const yaml__document_presentation__t * nonnull pres
)
{
    env->pres = t_yaml_doc_pres_to_map(pres);
}

int t_yaml_pack(yaml_pack_env_t * nonnull env,
                const yaml_data_t * nonnull data,
                yaml_pack_writecb_f * nonnull writecb, void * nullable priv,
                sb_t * nullable err)
{
    int res;

    env->write_cb = writecb;
    env->priv = priv;

    res = t_yaml_pack_data(env, data);
    if (res < 0 && err) {
        sb_setsb(err, &env->err);
    }

    return res;
}

static inline int sb_write(void * nonnull b, const void * nonnull buf,
                           int len, sb_t * nonnull err)
{
    sb_add(b, buf, len);
    return len;
}

int t_yaml_pack_sb(yaml_pack_env_t * nonnull env,
                   const yaml_data_t * nonnull data, sb_t * nonnull sb,
                   sb_t * nullable err)
{
    return t_yaml_pack(env, data, &sb_write, sb, err);
}

typedef struct yaml_pack_file_ctx_t {
    file_t *file;
} yaml_pack_file_ctx_t;

static int iop_ypack_write_file(void *priv, const void *data, int len,
                                sb_t *err)
{
    yaml_pack_file_ctx_t *ctx = priv;

    if (file_write(ctx->file, data, len) < 0) {
        sb_setf(err, "cannot write in output file: %m");
        return -1;
    }

    return len;
}

int
t_yaml_pack_file(yaml_pack_env_t * nonnull env, const char * nonnull filename,
                 const yaml_data_t * nonnull data, sb_t * nonnull err)
{
    char path[PATH_MAX];
    yaml_pack_file_ctx_t ctx;
    int res;

    if (env->outdirpath.len > 0) {
        filename = t_fmt("%pL/%s", &env->outdirpath, filename);
    }

    /* Make sure the outdirpath is the full dirpath, even if it was set
     * before. */
    path_dirname(path, PATH_MAX, filename);
    RETHROW(t_yaml_pack_env_set_outdir(env, path, err));

    p_clear(&ctx, 1);
    ctx.file = file_open(filename, env->file_flags, env->file_mode);
    if (!ctx.file) {
        sb_setf(err, "cannot open output file `%s`: %m", filename);
        return -1;
    }

    res = t_yaml_pack(env, data, &iop_ypack_write_file, &ctx, err);
    if (res < 0) {
        IGNORE(file_close(&ctx.file));
        return res;
    }

    /* End the file with a newline, as the packing ends immediately after
     * the last value. */
    if (env->state != PACK_STATE_ON_NEWLINE) {
        file_puts(ctx.file, "\n");
    }

    if (file_close(&ctx.file) < 0) {
        sb_setf(err, "cannot close output file `%s`: %m", filename);
        return -1;
    }

    return 0;
}

/* }}} */
/* {{{ AST helpers */

#define SET_SCALAR(data, scalar_type)                                        \
    do {                                                                     \
        p_clear(data, 1);                                                    \
        data->type = YAML_DATA_SCALAR;                                       \
        data->scalar.type = YAML_SCALAR_##scalar_type;                       \
    } while (0)

void yaml_data_set_string(yaml_data_t *data, lstr_t str)
{
    SET_SCALAR(data, STRING);
    data->scalar.s = str;
}

void yaml_data_set_double(yaml_data_t *data, double d)
{
    SET_SCALAR(data, DOUBLE);
    data->scalar.d = d;
}

void yaml_data_set_uint(yaml_data_t *data, uint64_t u)
{
    SET_SCALAR(data, UINT);
    data->scalar.u = u;
}

void yaml_data_set_int(yaml_data_t *data, int64_t i)
{
    SET_SCALAR(data, INT);
    data->scalar.i = i;
}

void yaml_data_set_bool(yaml_data_t *data, bool b)
{
    SET_SCALAR(data, BOOL);
    data->scalar.b = b;
}

void yaml_data_set_null(yaml_data_t *data)
{
    SET_SCALAR(data, NULL);
}

void t_yaml_data_new_seq(yaml_data_t *data, int capacity)
{
    p_clear(data, 1);
    data->type = YAML_DATA_SEQ;
    data->seq = t_new(yaml_seq_t, 1);
    t_qv_init(&data->seq->datas, capacity);
}

void yaml_seq_add_data(yaml_data_t *data, yaml_data_t val)
{
    assert (data->type == YAML_DATA_SEQ);
    qv_append(&data->seq->datas, val);
}

void t_yaml_data_new_obj(yaml_data_t *data, int capacity)
{
    p_clear(data, 1);
    data->type = YAML_DATA_OBJ;
    data->obj = t_new(yaml_obj_t, 1);
    t_qv_init(&data->obj->fields, capacity);
}

void yaml_obj_add_field(yaml_data_t *data, lstr_t key, yaml_data_t val)
{
    yaml_key_data_t *kd;

    assert (data->type == YAML_DATA_OBJ);
    kd = qv_growlen0(&data->obj->fields, 1);
    kd->key = key;
    kd->data = val;
}

/* }}} */
/* {{{ Module */

static int yaml_initialize(void *arg)
{
    return 0;
}

static int yaml_shutdown(void)
{
    return 0;
}

MODULE_BEGIN(yaml)
    /* There is an implicit dependency on "log" */
MODULE_END()

/* }}} */
/* {{{ Tests */

/* LCOV_EXCL_START */

#include <lib-common/z.h>

/* {{{ Helpers */

static int z_yaml_test_parse_fail(const char *yaml, const char *expected_err)
{
    t_scope;
    yaml_data_t data;
    yaml_parse_t *env = t_yaml_parse_new(0);
    SB_1k(err);

    yaml_parse_attach_ps(env, ps_initstr(yaml));
    Z_ASSERT_NEG(t_yaml_parse(env, &data, &err));
    Z_ASSERT_STREQUAL(err.data, expected_err,
                      "wrong error message on yaml string `%s`", yaml);
    yaml_parse_delete(&env);

    Z_HELPER_END;
}

static int
z_create_tmp_subdir(const char *dirpath)
{
    t_scope;
    const char *path;

    path = t_fmt("%pL/%s", &z_tmpdir_g, dirpath);
    Z_ASSERT_N(mkdir_p(path, 0777));

    Z_HELPER_END;
}

static int
z_write_yaml_file(const char *filepath, const char *yaml)
{
    t_scope;
    file_t *file;
    const char *path;

    path = t_fmt("%pL/%s", &z_tmpdir_g, filepath);
    file = file_open(path, FILE_WRONLY | FILE_CREATE | FILE_TRUNC, 0644);
    Z_ASSERT_P(file);

    file_puts(file, yaml);
    file_puts(file, "\n");

    Z_ASSERT_N(file_close(&file));

    Z_HELPER_END;
}

static int
z_pack_yaml_file(const char *filepath, const yaml_data_t *data,
                 const yaml__document_presentation__t *presentation,
                 unsigned flags)
{
    t_scope;
    yaml_pack_env_t *env;
    char *path;
    SB_1k(err);

    env = t_yaml_pack_env_new();
    if (flags) {
        yaml_pack_env_set_flags(env, flags);
    }
    path = t_fmt("%pL/%s", &z_tmpdir_g, filepath);
    if (presentation) {
        t_yaml_pack_env_set_presentation(env, presentation);
    }
    Z_ASSERT_N(t_yaml_pack_file(env, path, data, &err),
               "cannot pack YAML file %s: %pL", filepath, &err);

    Z_HELPER_END;
}

static int
z_pack_yaml_in_sb_with_subfiles(
    const char *dirpath, const yaml_data_t *data,
    const yaml__document_presentation__t *presentation,
    const char *expected_res)
{
    t_scope;
    yaml_pack_env_t *env;
    SB_1k(out);
    SB_1k(err);

    env = t_yaml_pack_env_new();
    dirpath = t_fmt("%pL/%s", &z_tmpdir_g, dirpath);
    Z_ASSERT_N(t_yaml_pack_env_set_outdir(env, dirpath, &err));
    if (presentation) {
        t_yaml_pack_env_set_presentation(env, presentation);
    }
    Z_ASSERT_N(t_yaml_pack_sb(env, data, &out, &err),
               "cannot pack YAML buffer: %pL", &err);
    Z_ASSERT_STREQUAL(out.data, expected_res);

    Z_HELPER_END;
}

static int z_check_file(const char *path, const char *expected_contents)
{
    t_scope;
    lstr_t contents;

    path = t_fmt("%pL/%s", &z_tmpdir_g, path);
    Z_ASSERT_N(lstr_init_from_file(&contents, path, PROT_READ, MAP_SHARED));
    Z_ASSERT_LSTREQUAL(contents, LSTR(expected_contents));
    lstr_wipe(&contents);

    Z_HELPER_END;
}

static int z_check_file_do_not_exist(const char *path)
{
    t_scope;

    path = t_fmt("%pL/%s", &z_tmpdir_g, path);
    Z_ASSERT_NEG(access(path, F_OK));

    Z_HELPER_END;
}

static int z_yaml_test_file_parse_fail(const char *yaml,
                                       const char *expected_err)
{
    t_scope;
    yaml_data_t data;
    yaml_parse_t *env = t_yaml_parse_new(0);
    SB_1k(err);

    Z_HELPER_RUN(z_write_yaml_file("input.yml", yaml));
    Z_ASSERT_N(t_yaml_parse_attach_file(env, "input.yml", z_tmpdir_g.s,
                                        &err),
               "%pL", &err);
    Z_ASSERT_NEG(t_yaml_parse(env, &data, &err));
    Z_ASSERT_STREQUAL(err.data, expected_err,
                      "wrong error message on yaml string `%s`", yaml);
    yaml_parse_delete(&env);

    Z_HELPER_END;
}

static int
z_yaml_test_pack(const yaml_data_t * nonnull data,
                 yaml__document_presentation__t * nullable pres,
                 unsigned flags, const char * nonnull expected_pack)
{
    yaml_pack_env_t *pack_env;
    SB_1k(pack);
    SB_1k(err);

    pack_env = t_yaml_pack_env_new();
    if (pres) {
        t_yaml_pack_env_set_presentation(pack_env, pres);
    }
    yaml_pack_env_set_flags(pack_env, flags);
    Z_ASSERT_N(t_yaml_pack_sb(pack_env, data, &pack, &err));
    Z_ASSERT_STREQUAL(pack.data, expected_pack,
                      "repacking the parsed data leads to differences");

    Z_HELPER_END;
}

/* out parameter first to let the yaml string be last, which makes it
 * much easier to write multiple lines without horrible indentation */
static
int z_t_yaml_test_parse_success(yaml_data_t * nullable data,
                                yaml__document_presentation__t *nullable pres,
                                yaml_parse_t * nonnull * nullable env,
                                const char * nonnull yaml,
                                const char * nullable expected_repack)
{
    yaml__document_presentation__t p;
    yaml_data_t local_data;
    yaml_parse_t *local_env = NULL;
    SB_1k(err);

    if (!pres) {
        pres = &p;
    }
    if (!data) {
        data = &local_data;
    }
    if (!env) {
        env = &local_env;
    }

    *env = t_yaml_parse_new(YAML_PARSE_GEN_PRES_DATA);
    /* hack to make relative inclusion work in z_tmpdir_g */
    (*env)->fullpath = t_lstr_fmt("%pL/foo.yml", &z_tmpdir_g);
    yaml_parse_attach_ps(*env, ps_initstr(yaml));
    Z_ASSERT_N(t_yaml_parse(*env, data, &err),
               "yaml parsing failed: %pL", &err);

    if (!expected_repack) {
        expected_repack = yaml;
    }

    /* repack using presentation data from the AST */
    Z_HELPER_RUN(z_yaml_test_pack(data, NULL, 0, expected_repack));

    /* repack using yaml_presentation_t specification, and not the
     * presentation data inside the AST */
    t_yaml_data_get_presentation(data, pres);
    Z_HELPER_RUN(z_yaml_test_pack(data, pres, 0, expected_repack));

    if (local_env) {
        yaml_parse_delete(&local_env);
    }

    Z_HELPER_END;
}

static int
z_check_yaml_span(const yaml_span_t *span,
                  uint32_t start_line, uint32_t start_col,
                  uint32_t end_line, uint32_t end_col)
{
    Z_ASSERT_EQ(span->start.line_nb, start_line);
    Z_ASSERT_EQ(span->start.col_nb, start_col);
    Z_ASSERT_EQ(span->end.line_nb, end_line);
    Z_ASSERT_EQ(span->end.col_nb, end_col);

    Z_HELPER_END;
}

static int
z_check_yaml_data(const yaml_data_t *data, yaml_data_type_t type,
                  uint32_t start_line, uint32_t start_col,
                  uint32_t end_line, uint32_t end_col)
{
    Z_ASSERT_EQ(data->type, type);
    Z_HELPER_RUN(z_check_yaml_span(&data->span, start_line, start_col,
                                   end_line, end_col));

    Z_HELPER_END;
}

static int
z_check_yaml_scalar(const yaml_data_t *data, yaml_scalar_type_t type,
                    uint32_t start_line, uint32_t start_col,
                    uint32_t end_line, uint32_t end_col)
{
    Z_HELPER_RUN(z_check_yaml_data(data, YAML_DATA_SCALAR, start_line,
                                   start_col, end_line, end_col));
    Z_ASSERT_EQ(data->scalar.type, type);

    Z_HELPER_END;
}

static int
z_check_yaml_pack(const yaml_data_t * nonnull data,
                  const yaml__document_presentation__t * nullable presentation,
                  const char *yaml)
{
    t_scope;
    yaml_pack_env_t *env = t_yaml_pack_env_new();
    SB_1k(sb);
    SB_1k(err);

    if (presentation) {
        t_yaml_pack_env_set_presentation(env, presentation);
    }
    Z_ASSERT_N(t_yaml_pack_sb(env, data, &sb, &err));
    Z_ASSERT_STREQUAL(sb.data, yaml);

    Z_HELPER_END;
}

static int z_check_inline_comment(const yaml_presentation_t * nonnull pres,
                                  lstr_t path, lstr_t comment)
{
    const yaml__presentation_node__t *pnode;

    pnode = qm_get_def_safe(yaml_pres_node, &pres->nodes, &path, NULL);
    Z_ASSERT_P(pnode);
    Z_ASSERT_LSTREQUAL(pnode->inline_comment, comment);

    Z_HELPER_END;
}

static int z_check_prefix_comments(const yaml_presentation_t * nonnull pres,
                                   lstr_t path, lstr_t *comments,
                                   int len)
{
    const yaml__presentation_node__t *pnode;

    pnode = qm_get_def_safe(yaml_pres_node, &pres->nodes, &path, NULL);
    Z_ASSERT_P(pnode);
    Z_ASSERT_EQ(len, pnode->prefix_comments.len);
    tab_for_each_pos(pos, &pnode->prefix_comments) {
        Z_ASSERT_LSTREQUAL(comments[pos], pnode->prefix_comments.tab[pos],
                           "prefix comment number #%d differs", pos);
    }

    Z_HELPER_END;
}

/* }}} */

Z_GROUP_EXPORT(yaml)
{
    MODULE_REQUIRE(yaml);

    /* {{{ Parsing errors */

    Z_TEST(parsing_errors, "errors when parsing yaml") {
        /* unexpected EOF */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "",
            "<string>:1:1: missing data, unexpected end of line"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "  # my comment",

            "<string>:1:15: missing data, unexpected end of line\n"
            "  # my comment\n"
            "              ^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "key:",

            "<string>:1:5: missing data, unexpected end of line\n"
            "key:\n"
            "    ^"
        ));

        /* wrong object continuation */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "a: 5\nb",

            "<string>:2:2: invalid key, missing colon\n"
            "b\n"
            " ^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "a: 5\n_:",

            "<string>:2:1: invalid key, "
            "only alpha-numeric characters allowed\n"
            "_:\n"
            "^"
        ));

        /* wrong explicit string */
        /* TODO: weird span? */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "\" unfinished string",

            "<string>:1:2: expected string, missing closing '\"'\n"
            "\" unfinished string\n"
            " ^"
        ));

        /* wrong escaped code */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "\"\\",

            "<string>:1:2: expected string, invalid backslash\n"
            "\"\\\n"
            " ^"
        ));

        /* wrong tag */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "!-",

            "<string>:1:2: invalid tag, must start with a letter\n"
            "!-\n"
            " ^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "!a-\n"
            "a: 5",

            "<string>:1:3: invalid tag, "
            "must only contain alphanumeric characters\n"
            "!a-\n"
            "  ^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "!4a\n"
            "a: 5",

            "<string>:1:2: invalid tag, must start with a letter\n"
            "!4a\n"
            " ^"
        ));
        /* TODO: improve span */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "!tag1\n"
            "!tag2\n"
            "a: 2",

            "<string>:3:5: wrong object, two tags have been declared\n"
            "a: 2\n"
            "    ^"
        ));

        /* wrong list continuation */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "- 2\n"
            "-3",

            "<string>:2:1: wrong type of data, "
            "expected another element of sequence\n"
            "-3\n"
            "^"
        ));

        /* wrong indent */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "a: 2\n"
            " b: 3",

            "<string>:2:2: wrong indentation, "
            "line not aligned with current object\n"
            " b: 3\n"
            " ^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "- 2\n"
            " - 3",

            "<string>:2:2: wrong indentation, "
            "line not aligned with current sequence\n"
            " - 3\n"
            " ^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "a: 1\n"
            "b:\n"
            "c: 3",

            "<string>:3:1: wrong indentation, missing element\n"
            "c: 3\n"
            "^"
        ));

        /* wrong object */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "foo: 1\n"
            "foo: 2",

            "<string>:2:1: invalid key, "
            "key is already declared in the object\n"
            "foo: 2\n"
            "^^^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "{ a: 1, a: 2}",

            "<string>:1:9: invalid key, "
            "key is already declared in the object\n"
            "{ a: 1, a: 2}\n"
            "        ^"
        ));

        /* cannot use tab characters for indentation */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "a:\t1",

            "<string>:1:3: tab character detected, "
            "cannot use tab characters for indentation\n"
            "a:\t1\n"
            "  ^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "a:\n"
            "\t- 2\n"
            "\t- 3",

            "<string>:2:1: tab character detected, "
            "cannot use tab characters for indentation\n"
            "\t- 2\n"
            "^"
        ));

        /* extra data after the parsing */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "1\n"
            "# comment\n"
            "2",

            "<string>:3:1: extra characters after data, "
            "expected end of document\n"
            "2\n"
            "^"
        ));

        /* flow seq */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "[a[",

            "<string>:1:3: wrong type of data, "
            "expected another element of sequence\n"
            "[a[\n"
            "  ^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "[",

            "<string>:1:2: missing data, unexpected end of line\n"
            "[\n"
            " ^"
        ));

        /* flow obj */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "{,",

            "<string>:1:2: missing data, unexpected character\n"
            "{,\n"
            " ^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "{a:b}",

            "<string>:1:2: wrong type of data, "
            "only key-value mappings are allowed inside an object\n"
            "{a:b}\n"
            " ^^^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "{a: b[",

            "<string>:1:6: wrong type of data, "
            "expected another element of object\n"
            "{a: b[\n"
            "     ^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "{ a: b: c }",

            "<string>:1:7: wrong type of data, unexpected colon\n"
            "{ a: b: c }\n"
            "      ^"
        ));

        /* Cannot use variables as keys outside of override context */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "$var: 3",

            "<string>:1:1: invalid key, "
            "cannot specify a variable value in this context\n"
            "$var: 3\n"
            "^^^^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "obj: { a: 2, $var: 3 }",

            "<string>:1:14: invalid key, "
            "cannot specify a variable value in this context\n"
            "obj: { a: 2, $var: 3 }\n"
            "             ^^^^"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "obj: [ $var: 3 ]",

            "<string>:1:8: invalid key, "
            "cannot specify a variable value in this context\n"
            "obj: [ $var: 3 ]\n"
            "       ^^^^"
        ));

        /* Unbound variables are rejected by default */
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "key: $var",

            "the document is invalid: there are unbound variables: var"
        ));
        Z_HELPER_RUN(z_yaml_test_parse_fail(
            "- $a\n"
            "- $boo",

            "the document is invalid: there are unbound variables: a, boo"
        ));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Parsing file errors */

    Z_TEST(parsing_file_errors, "errors when parsing YAML from files") {
        t_scope;
        yaml_parse_t *env;
        const char *path;
        const char *filename;
        SB_1k(err);

        /* unexpected EOF */
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "",
            "input.yml:2:1: missing data, unexpected end of line"
        ));

        env = t_yaml_parse_new(0);
        Z_ASSERT_NEG(t_yaml_parse_attach_file(env, "unknown.yml", NULL, &err));
        Z_ASSERT_STREQUAL(err.data, "cannot read file unknown.yml: "
                          "No such file or directory");

        /* create a file but make it unreadable */
        filename = "unreadable.yml";
        Z_HELPER_RUN(z_write_yaml_file(filename, "2"));
        path = t_fmt("%pL/%s", &z_tmpdir_g, filename);
        chmod(path, 220);

        Z_ASSERT_NEG(t_yaml_parse_attach_file(env, filename, z_tmpdir_g.s,
                                              &err));
        Z_ASSERT_STREQUAL(err.data, "cannot read file unreadable.yml: "
                          "Permission denied");
    } Z_TEST_END;

    /* }}} */
    /* {{{ Parsing file */

    Z_TEST(parsing_file, "test parsing YAML files") {
        t_scope;
        yaml_parse_t *env;
        const char *filename;
        yaml_data_t data;
        SB_1k(err);

        /* make sure including a file relative to "." works */
        filename = "rel_include.yml";
        Z_HELPER_RUN(z_write_yaml_file(filename, "2"));
        Z_ASSERT_N(chdir(z_tmpdir_g.s));

        env = t_yaml_parse_new(0);
        Z_ASSERT_N(t_yaml_parse_attach_file(env, filename, ".", &err));
        Z_ASSERT_N(t_yaml_parse(env, &data, &err));
        Z_ASSERT(data.type == YAML_DATA_SCALAR);
        Z_ASSERT(data.scalar.type == YAML_SCALAR_UINT);
        Z_ASSERT(data.scalar.u == 2);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Include errors */

    Z_TEST(include_errors, "errors when including YAML files") {
        /* !include tag must be applied on a string */
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "!include 3",

            "input.yml:1:1: invalid include, "
            "!include can only be used with strings\n"
            "!include 3\n"
            "^^^^^^^^^^"
        ));

        /* unknown file */
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "!include foo.yml",

            "input.yml:1:1: invalid include, "
            "cannot read file foo.yml: No such file or directory\n"
            "!include foo.yml\n"
            "^^^^^^^^^^^^^^^^"
        ));

        /* error in included file */
        Z_HELPER_RUN(z_write_yaml_file("has_errors.yml",
            "key: 1\n"
            "key: 2"
        ));
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "!include has_errors.yml",

            "input.yml:1:1: error in included file\n"
            "!include has_errors.yml\n"
            "^^^^^^^^^^^^^^^^^^^^^^^\n"
            "has_errors.yml:2:1: invalid key, "
            "key is already declared in the object\n"
            "key: 2\n"
            "^^^"
        ));

        /* loop detection: include one-self */
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "!include input.yml",

            "input.yml:1:1: invalid include, inclusion loop detected\n"
            "!include input.yml\n"
            "^^^^^^^^^^^^^^^^^^"
        ));
        /* loop detection: include a parent */
        Z_HELPER_RUN(z_write_yaml_file("loop-1.yml",
            "!include loop-2.yml"
        ));
        Z_HELPER_RUN(z_write_yaml_file("loop-2.yml",
            "!include loop-3.yml"
        ));
        Z_HELPER_RUN(z_write_yaml_file("loop-3.yml",
            "!include loop-1.yml"
        ));
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "!include loop-1.yml",

            "input.yml:1:1: error in included file\n"
            "!include loop-1.yml\n"
            "^^^^^^^^^^^^^^^^^^^\n"
            "loop-1.yml:1:1: error in included file\n"
            "!include loop-2.yml\n"
            "^^^^^^^^^^^^^^^^^^^\n"
            "loop-2.yml:1:1: error in included file\n"
            "!include loop-3.yml\n"
            "^^^^^^^^^^^^^^^^^^^\n"
            "loop-3.yml:1:1: invalid include, inclusion loop detected\n"
            "!include loop-1.yml\n"
            "^^^^^^^^^^^^^^^^^^^"
        ));

        /* includes must be in same directory as including file */
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "!include ../input.yml",

            "input.yml:1:1: invalid include, cannot include subfile "
            "`../input.yml`: only includes contained in the directory of the "
            "including file are allowed\n"
            "!include ../input.yml\n"
            "^^^^^^^^^^^^^^^^^^^^^"
        ));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Include */

    Z_TEST(include, "") {
        t_scope;

        Z_HELPER_RUN(z_write_yaml_file("inner.yml",
            "- a: 3\n"
            "  b: { c: c }\n"
            "- true"
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(NULL, NULL, NULL,
            "a: ~\n"
            "b: !include inner.yml\n"
            "c: 3",

            "a: ~\n"
            "b:\n"
            "  - a: 3\n"
            "    b: { c: c }\n"
            "  - true\n"
            "c: 3"
        ));

        Z_HELPER_RUN(z_create_tmp_subdir("subdir/subsub"));
        Z_HELPER_RUN(z_write_yaml_file("subdir/a.yml",
            "- a\n"
            "- !include b.yml\n"
            "- !include subsub/d.yml"
        ));
        Z_HELPER_RUN(z_write_yaml_file("subdir/b.yml",
            "- !include subsub/c.yml\n"
            "- b"
        ));
        /* TODO d.yml is included twice, should be factorized instead of
         * parsing the file twice. */
        Z_HELPER_RUN(z_write_yaml_file("subdir/subsub/c.yml",
            "- c\n"
            "- !include d.yml"
        ));
        Z_HELPER_RUN(z_write_yaml_file("subdir/subsub/d.yml",
            "d"
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(NULL, NULL, NULL,
            "!include subdir/a.yml",

            "- a\n"
            "- - - c\n"
            "    - d\n"
            "  - b\n"
            "- d"
        ));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Include shared files */

    Z_TEST(include_shared_files, "") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t pres;
        yaml_parse_t *env;

        Z_HELPER_RUN(z_create_tmp_subdir("sf/sub"));
        Z_HELPER_RUN(z_write_yaml_file("sf/shared_1.yml", "1"));
        Z_HELPER_RUN(z_write_yaml_file("sf/sub/shared_1.yml", "-1"));
        Z_HELPER_RUN(z_write_yaml_file("sf/shared_2",
            "!include sub/shared_1.yml"
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            "- !include sf/shared_1.yml\n"
            "- !include sf/././shared_1.yml\n"
            "- !include sf/shared_1.yml\n"
            "- !include sf/sub/shared_1.yml\n"
            "- !include sf/../sf/sub/shared_1.yml\n"
            "- !include sf/sub/shared_1.yml\n"
            "- !include sf/shared_2\n"
            "- !include ./sf/shared_2",

            "- 1\n"
            "- 1\n"
            "- 1\n"
            "- -1\n"
            "- -1\n"
            "- -1\n"
            "- -1\n"
            "- -1"
        ));

        /* repacking it will shared the same subfiles */
        Z_HELPER_RUN(z_create_tmp_subdir("sf-pack-1"));
        Z_HELPER_RUN(z_pack_yaml_file("sf-pack-1/root.yml", &data, &pres, 0));
        Z_HELPER_RUN(z_check_file("sf-pack-1/root.yml",
            "- !include sf/shared_1.yml\n"
            "- !include sf/shared_1.yml\n"
            "- !include sf/shared_1.yml\n"
            "- !include sf/sub/shared_1.yml\n"
            "- !include sf/sub/shared_1.yml\n"
            "- !include sf/sub/shared_1.yml\n"
            "- !include sf/shared_2\n"
            "- !include sf/shared_2\n"
        ));
        Z_HELPER_RUN(z_check_file("sf-pack-1/sf/shared_1.yml", "1\n"));
        Z_HELPER_RUN(z_check_file("sf-pack-1/sf/sub/shared_1.yml", "-1\n"));
        Z_HELPER_RUN(z_check_file("sf-pack-1/sf/shared_2",
            "!include sub/shared_1.yml\n"
        ));

        /* modifying some data will force the repacking to create new files */
        data.seq->datas.tab[1].scalar.u = 2;
        data.seq->datas.tab[2].scalar.u = 2;
        data.seq->datas.tab[4].scalar.i = -2;
        data.seq->datas.tab[5].scalar.i = -3;
        data.seq->datas.tab[7].scalar.i = -3;
        Z_HELPER_RUN(z_create_tmp_subdir("sf-pack-2"));
        Z_HELPER_RUN(z_pack_yaml_file("sf-pack-2/root.yml", &data, &pres, 0));
        Z_HELPER_RUN(z_check_file("sf-pack-2/root.yml",
            "- !include sf/shared_1.yml\n"
            "- !include sf/shared_1~1.yml\n"
            "- !include sf/shared_1~1.yml\n"
            "- !include sf/sub/shared_1.yml\n"
            "- !include sf/sub/shared_1~1.yml\n"
            "- !include sf/sub/shared_1~2.yml\n"
            "- !include sf/shared_2\n"
            "- !include sf/shared_2~1\n"
        ));
        Z_HELPER_RUN(z_check_file("sf-pack-2/sf/shared_1.yml", "1\n"));
        Z_HELPER_RUN(z_check_file("sf-pack-2/sf/shared_1~1.yml", "2\n"));
        Z_HELPER_RUN(z_check_file("sf-pack-2/sf/sub/shared_1.yml", "-1\n"));
        Z_HELPER_RUN(z_check_file("sf-pack-2/sf/sub/shared_1~1.yml", "-2\n"));
        Z_HELPER_RUN(z_check_file("sf-pack-2/sf/sub/shared_1~2.yml", "-3\n"));
        Z_HELPER_RUN(z_check_file("sf-pack-2/sf/shared_2",
            "!include sub/shared_1.yml\n"
        ));
        Z_HELPER_RUN(z_check_file("sf-pack-2/sf/shared_2~1",
            "!include sub/shared_1~2.yml\n"
        ));
        yaml_parse_delete(&env);

    } Z_TEST_END;

    /* }}} */
    /* {{{ Include presentation */

    Z_TEST(include_presentation, "") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t pres;
        yaml_parse_t *env;

        Z_HELPER_RUN(z_create_tmp_subdir("subpres/in"));
        Z_HELPER_RUN(z_write_yaml_file("subpres/1.yml",
            "# Included!\n"
            "!include in/sub.yml"
        ));
        Z_HELPER_RUN(z_write_yaml_file("subpres/in/sub.yml",
            "[ 4, 2 ] # packed"
        ));
        Z_HELPER_RUN(z_write_yaml_file("subpres/weird~name",
            "jo: Jo\n"
            "# o\n"
            "o: ra"
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            "- !include subpres/1.yml\n"
            "- !include subpres/weird~name",

            /* XXX: the presentation associated with the "!include" data is
             * not included, as the data is inlined. */
            "- [ 4, 2 ] # packed\n"
            "- jo: Jo\n"
            "  # o\n"
            "  o: ra"
        ));

        Z_HELPER_RUN(z_create_tmp_subdir("newsubdir/in"));
        Z_HELPER_RUN(z_pack_yaml_file("newsubdir/root.yml", &data, &pres, 0));
        Z_HELPER_RUN(z_check_file("newsubdir/root.yml",
            "- !include subpres/1.yml\n"
            "- !include subpres/weird~name\n"
        ));
        Z_HELPER_RUN(z_check_file("newsubdir/subpres/1.yml",
            "# Included!\n"
            "!include in/sub.yml\n"
        ));
        Z_HELPER_RUN(z_check_file("newsubdir/subpres/in/sub.yml",
            "[ 4, 2 ] # packed\n"
        ));
        Z_HELPER_RUN(z_check_file("newsubdir/subpres/weird~name",
            "jo: Jo\n"
            "# o\n"
            "o: ra\n"
        ));
        yaml_parse_delete(&env);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Include raw */

    Z_TEST(include_raw, "") {
        t_scope;
        yaml_data_t data;
        yaml_data_t new_data;
        yaml_data_t bool_data;
        yaml__document_presentation__t pres;
        yaml_parse_t *env;

        /* Write a JSON file */
        Z_HELPER_RUN(z_create_tmp_subdir("raw"));
        Z_HELPER_RUN(z_write_yaml_file("raw/inner.json",
            "{\n"
            "  \"foo\": 2\n"
            "}"
        ));
        /* include it verbatim as a string in a YAML document */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            "- !includeraw raw/inner.json",

            "- \"{\\n  \\\"foo\\\": 2\\n}\\n\""
        ));

        /* check repacking with presentation */
        Z_HELPER_RUN(z_pack_yaml_file("packraw/root.yml", &data, &pres, 0));
        Z_HELPER_RUN(z_check_file("packraw/root.yml",
            "- !includeraw raw/inner.json\n"
        ));
        Z_HELPER_RUN(z_check_file("packraw/raw/inner.json",
            "{\n"
            "  \"foo\": 2\n"
            "}\n"
        ));

        /* if the included data is no longer a string, it will be dumped as
         * a classic include. */
        t_yaml_data_new_obj(&new_data, 2);
        yaml_obj_add_field(&new_data, LSTR("json"), data.seq->datas.tab[0]);
        yaml_data_set_bool(&bool_data, true);
        yaml_obj_add_field(&new_data, LSTR("b"), bool_data);
        data.seq->datas.tab[0] = new_data;
        Z_HELPER_RUN(z_pack_yaml_file("packraw2/root.yml", &data, &pres, 0));
        Z_HELPER_RUN(z_check_file("packraw2/root.yml",
            /* TODO: we still keep the same file extension, which isn't ideal.
             * Maybe adding a .yml on top of it (without removing the old
             * extension) would be better, maybe even adding a prefix comment
             * for the include explaining the file could no longer be packed
             * raw. */
            "- !include raw/inner.json\n"
        ));
        Z_HELPER_RUN(z_check_file("packraw2/raw/inner.json",
            "json: \"{\\n  \\\"foo\\\": 2\\n}\\n\"\n"
            "b: true\n"
        ));

        yaml_parse_delete(&env);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Override */

    Z_TEST(override, "") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t pres;
        yaml_parse_t *env;
        const char *grandchild;
        const char *child;
        const char *root;

        /* test override of scalars, object, sequence */
        Z_HELPER_RUN(z_write_yaml_file("inner.yml",
            "a: 3\n"
            "b: { c: c }\n"
            "c:\n"
            "  - 3\n"
            "  - 4"
        ));
        root =
            "- !include inner.yml\n"
            "  a: 4\n"
            "\n"
            "  b: { new: true, c: ~ }\n"
            "  c: [ 5, 6 ] # array\n"
            "  # prefix d\n"
            "  d: ~";
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            root,

            "- a: 4\n"
            "  b: { c: ~, new: true }\n"
            "  c:\n"
            "    - 3\n"
            "    - 4\n"
            "    - 5\n"
            "    - 6\n"
            "  d: ~"
        ));
        /* test recreation of override when packing into files */
        Z_HELPER_RUN(z_pack_yaml_file("override_1/root.yml", &data, &pres,
                                      0));
        Z_HELPER_RUN(z_check_file("override_1/root.yml",
                                  t_fmt("%s\n", root)));
        Z_HELPER_RUN(z_check_file("override_1/inner.yml",
            /* XXX: lost flow mode, incompatible with override */
            "a: 3\n"
            "b:\n"
            "  c: c\n"
            "c:\n"
            "  - 3\n"
            "  - 4\n"
        ));
        Z_HELPER_RUN(z_check_file("override_1/root.yml",
                                  t_fmt("%s\n", root)));
        Z_HELPER_RUN(z_yaml_test_pack(&data, &pres, YAML_PACK_NO_SUBFILES,
                                      root));
        yaml_parse_delete(&env);

        /* test override of override through includes */
        grandchild =
            "# prefix gc a\n"
            "a: 1 # inline gc 1\n"
            "# prefix gc b\n"
            "b: 2 # inline gc 2\n"
            "# prefix gc c\n"
            "c: 3 # inline gc 3\n"
            "# prefix gc d\n"
            "d: 4 # inline gc 4\n";
        Z_HELPER_RUN(z_write_yaml_file("grandchild.yml", grandchild));
        child =
            "# prefix child g\n"
            "g: !include grandchild.yml # inline include\n"
            "  # prefix child c\n"
            "  c: 5 # inline child 5\n"
            "  # prefix child d\n"
            "  d: 6 # inline child 6\n";
        Z_HELPER_RUN(z_write_yaml_file("child.yml", child));
        root =
            "# prefix seq\n"
            "- !include child.yml\n"
            "  # prefix g\n"
            "  g: # inline g\n"
            "    # prefix b\n"
            "    b: 7 # inline 7\n"
            "    # prefix c\n"
            "    c: 8 # inline 8\n";
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            root,

            /* XXX: the presentation of the original file is used. This is
             * a side effect of how the presentation is stored, but it isn't
             * clear what would be the right behavior. Both have sensical
             * meaning. */
            "# prefix seq\n"
            "-\n"
            "  # prefix child g\n"
            "  g:\n"
            "    # prefix gc a\n"
            "    a: 1 # inline gc 1\n"
            "    # prefix gc b\n"
            "    b: 7 # inline gc 2\n"
            "    # prefix gc c\n"
            "    c: 8 # inline gc 3\n"
            "    # prefix gc d\n"
            "    d: 6 # inline gc 4\n"
        ));

        /* test recreation of override when packing into files */
        Z_HELPER_RUN(z_yaml_test_pack(&data, &pres, YAML_PACK_NO_SUBFILES,
                                      root));
        Z_HELPER_RUN(z_pack_yaml_file("override_2/root.yml", &data, &pres,
                                      0));
        Z_HELPER_RUN(z_check_file("override_2/grandchild.yml",
                                  grandchild));
        Z_HELPER_RUN(z_check_file("override_2/child.yml",
                                  child));
        Z_HELPER_RUN(z_check_file("override_2/root.yml",
                                  root));
        yaml_parse_delete(&env);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Override errors */

    Z_TEST(override_errors, "") {
        t_scope;

        Z_HELPER_RUN(z_write_yaml_file("inner.yml",
            "a: { b: { c: { d: { e: ~ } } } }"
        ));

        /* only objects allowed as overrides */
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "key: !include inner.yml\n"
            "  - 1\n"
            "  - 2",

            "input.yml:2:3: wrong indentation, "
            "line not aligned with current object\n"
            "  - 1\n"
            "  ^"
        ));
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "key: !include inner.yml\n"
            "   true",

            "input.yml:2:4: wrong indentation, "
            "line not aligned with current object\n"
            "   true\n"
            "   ^"
        ));

        /* must have same type as overridden data */
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "key: !include inner.yml\n"
            "  a:\n"
            "    b:\n"
            "      c:\n"
            "        - 1",

            "input.yml:5:9: cannot change types of data in override, "
            "overridden data is an object and not a sequence\n"
            "        - 1\n"
            "        ^^^"
        ));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Override conflict handling */

    Z_TEST(override_conflict_handling, "") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t pres;
        yaml_parse_t *env;
        const char *root;

        /* Test removal of added node */
        Z_HELPER_RUN(z_write_yaml_file("inner.yml",
            "a: 1\n"
            "b: 2"
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            "- !include inner.yml\n"
            "  b: 3\n"
            "  c: 4",

            "- a: 1\n"
            "  b: 3\n"
            "  c: 4"
        ));

        /* modify value in the AST */
        data.seq->datas.tab[0].obj->fields.tab[1].data.scalar.u = 10;
        data.seq->datas.tab[0].obj->fields.tab[2].data.scalar.u = 20;

        /* This value should get resolved in the override */
        root =
            "- !include inner.yml\n"
            "  b: 10\n"
            "  c: 20";
        Z_HELPER_RUN(z_pack_yaml_in_sb_with_subfiles("conflicts_1", &data,
                                                     &pres, root));
        Z_HELPER_RUN(z_check_file("conflicts_1/inner.yml",
            "a: 1\n"
            "b: 2\n"
        ));
        Z_HELPER_RUN(z_yaml_test_pack(&data, &pres, YAML_PACK_NO_SUBFILES,
                                      root));

        /* remove added node from AST */
        data.seq->datas.tab[0].obj->fields.len--;

        /* When packing into files, the override is normally recreated, but
         * here a node is removed. */
        Z_HELPER_RUN(z_pack_yaml_in_sb_with_subfiles("conflicts_2", &data,
                                                     &pres,
            "- !include inner.yml\n"
            "  b: 10"
        ));
        Z_HELPER_RUN(z_check_file("conflicts_2/inner.yml",
            "a: 1\n"
            "b: 2\n"
        ));

        /* Remove node b as well. This will remove the override entirely. */
        data.seq->datas.tab[0].obj->fields.len--;
        Z_HELPER_RUN(z_pack_yaml_in_sb_with_subfiles("conflicts_3", &data,
                                                     &pres,
            "- !include inner.yml"
        ));
        Z_HELPER_RUN(z_check_file("conflicts_3/inner.yml",
            "a: 1\n"
        ));
        yaml_parse_delete(&env);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Override shared subfiles */

    Z_TEST(override_shared_subfiles, "") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t pres;
        yaml_parse_t *env;
        const char *root;

        Z_HELPER_RUN(z_write_yaml_file("grandchild.yml",
            "a: a\n"
            "b: b"
        ));
        Z_HELPER_RUN(z_write_yaml_file("child.yml",
            "!include grandchild.yml\n"
            "b: B"
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            "- !include child.yml\n"
            "  a: 0\n"
            "- !include child.yml\n"
            "  a: 1\n"
            "- !include child.yml\n"
            "  b: 2",

            "- a: 0\n"
            "  b: B\n"
            "- a: 1\n"
            "  b: B\n"
            "- a: a\n"
            "  b: 2"
        ));

        /* repack into a file: the included subfiles should be shared */
        root =
            "- !include child.yml\n"
            "  a: 0\n"
            "- !include child.yml\n"
            "  a: 1\n"
            "- !include child.yml\n"
            "  b: 2";
        Z_HELPER_RUN(z_pack_yaml_in_sb_with_subfiles("override_shared_1",
                                                     &data, &pres, root));
        Z_HELPER_RUN(z_check_file("override_shared_1/child.yml",
            "!include grandchild.yml\n"
            "b: B\n"
        ));
        Z_HELPER_RUN(z_check_file("override_shared_1/grandchild.yml",
            "a: a\n"
            "b: b\n"
        ));
        Z_HELPER_RUN(z_yaml_test_pack(&data, &pres, YAML_PACK_NO_SUBFILES,
                                      root));

        /* modify [0].b. This will modify its child, but the grandchild is
         * still shared. */
        data.seq->datas.tab[0].obj->fields.tab[1].data.scalar.s = LSTR("B2");
        Z_HELPER_RUN(z_pack_yaml_in_sb_with_subfiles("override_shared_2",
                                                     &data, &pres,
            "- !include child.yml\n"
            "  a: 0\n"
            "- !include child~1.yml\n"
            "  a: 1\n"
            "- !include child~1.yml\n"
            "  b: 2"
        ));
        Z_HELPER_RUN(z_check_file("override_shared_2/child.yml",
            "!include grandchild.yml\n"
            "b: B2\n"
        ));
        Z_HELPER_RUN(z_check_file("override_shared_2/child~1.yml",
            "!include grandchild.yml\n"
            "b: B\n"
        ));
        Z_HELPER_RUN(z_check_file("override_shared_2/grandchild.yml",
            "a: a\n"
            "b: b\n"
        ));
        /* When packing with NO_SUBFILES, we do not check for collisions,
         * so the include path are kept. */
        Z_HELPER_RUN(z_yaml_test_pack(&data, &pres, YAML_PACK_NO_SUBFILES,
                                      root));

        /* reset [0].b, and modify [2].a. The grandchild will differ, but the
         * child is the same */
        data.seq->datas.tab[0].obj->fields.tab[1].data.scalar.s = LSTR("B");
        data.seq->datas.tab[2].obj->fields.tab[0].data.scalar.s = LSTR("A");

        Z_HELPER_RUN(z_pack_yaml_in_sb_with_subfiles("override_shared_2",
                                                     &data, &pres,
            "- !include child.yml\n"
            "  a: 0\n"
            "- !include child.yml\n"
            "  a: 1\n"
            "- !include child~1.yml\n"
            "  b: 2"
        ));
        Z_HELPER_RUN(z_check_file("override_shared_2/child.yml",
            "!include grandchild.yml\n"
            "b: B\n"
        ));
        Z_HELPER_RUN(z_check_file("override_shared_2/child~1.yml",
            "!include grandchild~1.yml\n"
            "b: B\n"
        ));
        Z_HELPER_RUN(z_check_file("override_shared_2/grandchild.yml",
            "a: a\n"
            "b: b\n"
        ));
        Z_HELPER_RUN(z_check_file("override_shared_2/grandchild~1.yml",
            "a: A\n"
            "b: b\n"
        ));
        Z_HELPER_RUN(z_yaml_test_pack(&data, &pres, YAML_PACK_NO_SUBFILES,
                                      root));

        yaml_parse_delete(&env);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Parsing scalars */

    Z_TEST(parsing_scalar, "test parsing of scalars") {
        t_scope;
        yaml_data_t data;

        /* string */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "unquoted string",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_STRING,
                                         1, 1, 1, 16));
        Z_ASSERT_LSTREQUAL(data.scalar.s, LSTR("unquoted string"));
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a string value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "!tag unquoted string",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_STRING,
                                         1, 1, 1, 21));
        Z_ASSERT_LSTREQUAL(data.scalar.s, LSTR("unquoted string"));
        Z_ASSERT_LSTREQUAL(data.tag, LSTR("tag"));
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a tagged string value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "\" quoted: 5 \"",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_STRING,
                                         1, 1, 1, 14));
        Z_ASSERT_LSTREQUAL(data.scalar.s, LSTR(" quoted: 5 "));

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "  trimmed   ",
            "trimmed"
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_STRING,
                                         1, 3, 1, 10));
        Z_ASSERT_LSTREQUAL(data.scalar.s, LSTR("trimmed"));

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "a:x:b",
            "\"a:x:b\""
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_STRING,
                                         1, 1, 1, 6));
        Z_ASSERT_LSTREQUAL(data.scalar.s, LSTR("a:x:b"));

        /* null */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "~",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_NULL,
                                         1, 1, 1, 2));
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a null value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "!tag ~",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_NULL,
                                         1, 1, 1, 7));
        Z_ASSERT_LSTREQUAL(data.tag, LSTR("tag"));
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a tagged null value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "null",
            "~"
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_NULL,
                                         1, 1, 1, 5));

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "NulL",
            "~"
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_NULL,
                                         1, 1, 1, 5));

        /* bool */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "true",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_BOOL,
                                         1, 1, 1, 5));
        Z_ASSERT(data.scalar.b);
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a boolean value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "!tag true",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_BOOL,
                                         1, 1, 1, 10));
        Z_ASSERT(data.scalar.b);
        Z_ASSERT_LSTREQUAL(data.tag, LSTR("tag"));
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a tagged boolean value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "TrUE",
            "true"
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_BOOL,
                                         1, 1, 1, 5));
        Z_ASSERT(data.scalar.b);

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "false",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_BOOL,
                                         1, 1, 1, 6));
        Z_ASSERT(!data.scalar.b);

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "FALse",
            "false"
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_BOOL,
                                         1, 1, 1, 6));
        Z_ASSERT(!data.scalar.b);

        /* uint */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "0",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_UINT,
                                         1, 1, 1, 2));
        Z_ASSERT_EQ(data.scalar.u, 0UL);
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "an unsigned integer value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "!tag 0",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_UINT,
                                         1, 1, 1, 7));
        Z_ASSERT_EQ(data.scalar.u, 0UL);
        Z_ASSERT_LSTREQUAL(data.tag, LSTR("tag"));
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a tagged unsigned integer value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "153",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_UINT,
                                         1, 1, 1, 4));
        Z_ASSERT_EQ(data.scalar.u, 153UL);

        /* -0 will still generate UINT */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "-0",
            "0"
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_UINT,
                                         1, 1, 1, 3));

        /* int */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "-1",
            NULL));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_INT,
                                         1, 1, 1, 3));
        Z_ASSERT_EQ(data.scalar.i, -1L);
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "an integer value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "!tag -1",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_INT,
                                         1, 1, 1, 8));
        Z_ASSERT_EQ(data.scalar.i, -1L);
        Z_ASSERT_LSTREQUAL(data.tag, LSTR("tag"));
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a tagged integer value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "-153",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_INT,
                                         1, 1, 1, 5));
        Z_ASSERT_EQ(data.scalar.i, -153L);

        /* double */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "0.5",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_DOUBLE,
                                         1, 1, 1, 4));
        Z_ASSERT_EQ(data.scalar.d, 0.5);
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a double value");

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "!tag 0.5",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_DOUBLE,
                                         1, 1, 1, 9));
        Z_ASSERT_EQ(data.scalar.d, 0.5);
        Z_ASSERT_LSTREQUAL(data.tag, LSTR("tag"));
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a tagged double value");

        /* TODO: should a dot be added to show its a floating-point number. */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "-1e3",
            "-1000"
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_DOUBLE,
                                         1, 1, 1, 5));
        Z_ASSERT_EQ(data.scalar.d, -1000.0);

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "-.Inf",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_DOUBLE,
                                         1, 1, 1, 6));
        Z_ASSERT_EQ(isinf(data.scalar.d), -1);

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            ".INf",
            ".Inf"
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_DOUBLE,
                                         1, 1, 1, 5));
        Z_ASSERT_EQ(isinf(data.scalar.d), 1);

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            ".NAN",
            ".NaN"
        ));
        Z_HELPER_RUN(z_check_yaml_scalar(&data, YAML_SCALAR_DOUBLE,
                                         1, 1, 1, 5));
        Z_ASSERT(isnan(data.scalar.d));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Parsing objects */

    Z_TEST(parsing_obj, "test parsing of objects") {
        t_scope;
        yaml_data_t data;
        yaml_data_t field;
        yaml_data_t field2;

        logger_set_level(LSTR("yaml"), LOG_TRACE + 2, 0);

        /* one liner */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "a: 2",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_OBJ, 1, 1, 1, 5));
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT(data.obj->fields.len == 1);
        Z_ASSERT_LSTREQUAL(data.obj->fields.tab[0].key, LSTR("a"));
        Z_HELPER_RUN(z_check_yaml_span(&data.obj->fields.tab[0].key_span,
                                       1, 1, 1, 2));
        field = data.obj->fields.tab[0].data;
        Z_HELPER_RUN(z_check_yaml_scalar(&field, YAML_SCALAR_UINT,
                                         1, 4, 1, 5));
        Z_ASSERT_EQ(field.scalar.u, 2UL);
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "an object");

        /* with tag */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "!tag1 a: 2",

            "!tag1\n"
            "a: 2"
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_OBJ, 1, 1, 1, 11));
        Z_ASSERT_LSTREQUAL(data.tag, LSTR("tag1"));
        Z_ASSERT(data.obj->fields.len == 1);
        Z_ASSERT_LSTREQUAL(data.obj->fields.tab[0].key, LSTR("a"));
        Z_HELPER_RUN(z_check_yaml_span(&data.obj->fields.tab[0].key_span,
                                       1, 7, 1, 8));
        field = data.obj->fields.tab[0].data;
        Z_HELPER_RUN(z_check_yaml_scalar(&field, YAML_SCALAR_UINT,
                                         1, 10, 1, 11));
        Z_ASSERT_EQ(field.scalar.u, 2UL);
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a tagged object");

        /* imbricated objects */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "a: 2\n"
            "inner: b: 3\n"
            "       c: -4\n"
            "inner2: !tag\n"
            "  d: ~\n"
            "  e: my-label\n"
            "f: 1.2",

            "a: 2\n"
            "inner:\n"
            "  b: 3\n"
            "  c: -4\n"
            "inner2: !tag\n"
            "  d: ~\n"
            "  e: my-label\n"
            "f: 1.2"
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_OBJ, 1, 1, 7, 7));
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT(data.obj->fields.len == 4);

        /* a */
        Z_ASSERT_LSTREQUAL(data.obj->fields.tab[0].key, LSTR("a"));
        field = data.obj->fields.tab[0].data;
        Z_HELPER_RUN(z_check_yaml_scalar(&field, YAML_SCALAR_UINT,
                                         1, 4, 1, 5));
        Z_ASSERT_EQ(field.scalar.u, 2UL);

        /* inner */
        Z_ASSERT_LSTREQUAL(data.obj->fields.tab[1].key, LSTR("inner"));
        Z_HELPER_RUN(z_check_yaml_span(&data.obj->fields.tab[1].key_span,
                                       2, 1, 2, 6));
        field = data.obj->fields.tab[1].data;
        Z_HELPER_RUN(z_check_yaml_data(&field, YAML_DATA_OBJ, 2, 8, 3, 13));
        Z_ASSERT_NULL(field.tag.s);
        Z_ASSERT(field.obj->fields.len == 2);

        Z_ASSERT_LSTREQUAL(field.obj->fields.tab[0].key, LSTR("b"));
        Z_HELPER_RUN(z_check_yaml_span(&field.obj->fields.tab[0].key_span,
                                       2, 8, 2, 9));
        field2 = field.obj->fields.tab[0].data;
        Z_HELPER_RUN(z_check_yaml_scalar(&field2, YAML_SCALAR_UINT,
                                         2, 11, 2, 12));
        Z_ASSERT_EQ(field2.scalar.u, 3UL);
        Z_ASSERT_LSTREQUAL(field.obj->fields.tab[1].key, LSTR("c"));
        Z_HELPER_RUN(z_check_yaml_span(&field.obj->fields.tab[1].key_span,
                                       3, 8, 3, 9));
        field2 = field.obj->fields.tab[1].data;
        Z_HELPER_RUN(z_check_yaml_scalar(&field2, YAML_SCALAR_INT,
                                         3, 11, 3, 13));
        Z_ASSERT_EQ(field2.scalar.i, -4L);

        /* inner2 */
        Z_ASSERT_LSTREQUAL(data.obj->fields.tab[2].key, LSTR("inner2"));
        Z_HELPER_RUN(z_check_yaml_span(&data.obj->fields.tab[2].key_span,
                                       4, 1, 4, 7));
        field = data.obj->fields.tab[2].data;
        Z_HELPER_RUN(z_check_yaml_data(&field, YAML_DATA_OBJ, 4, 9, 6, 14));
        Z_ASSERT_LSTREQUAL(field.tag, LSTR("tag"));
        Z_ASSERT(field.obj->fields.len == 2);

        Z_ASSERT_LSTREQUAL(field.obj->fields.tab[0].key, LSTR("d"));
        field2 = field.obj->fields.tab[0].data;
        Z_HELPER_RUN(z_check_yaml_scalar(&field2, YAML_SCALAR_NULL,
                                         5, 6, 5, 7));
        Z_ASSERT_LSTREQUAL(field.obj->fields.tab[1].key, LSTR("e"));
        field2 = field.obj->fields.tab[1].data;
        Z_HELPER_RUN(z_check_yaml_scalar(&field2, YAML_SCALAR_STRING,
                                         6, 6, 6, 14));
        Z_ASSERT_LSTREQUAL(field2.scalar.s, LSTR("my-label"));

        /* f */
        Z_ASSERT_LSTREQUAL(data.obj->fields.tab[3].key, LSTR("f"));
        field = data.obj->fields.tab[3].data;
        Z_HELPER_RUN(z_check_yaml_scalar(&field, YAML_SCALAR_DOUBLE,
                                         7, 4, 7, 7));
        Z_ASSERT_EQ(field.scalar.d, 1.2);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Parsing sequences */

    Z_TEST(parsing_seq, "test parsing of sequences") {
        t_scope;
        yaml_data_t data;
        yaml_data_t elem;

        /* one liner */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "- a",
            NULL
        ));
        Z_ASSERT_NULL(data.tag.s);
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_SEQ, 1, 1, 1, 4));
        Z_ASSERT_EQ(data.seq->datas.len, 1);
        Z_HELPER_RUN(z_check_yaml_scalar(&data.seq->datas.tab[0],
                                         YAML_SCALAR_STRING, 1, 3, 1, 4));
        Z_ASSERT_LSTREQUAL(data.seq->datas.tab[0].scalar.s, LSTR("a"));
        Z_ASSERT_STREQUAL(yaml_data_get_type(&data, false),
                          "a sequence");

        /* imbricated sequences */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "- \"a: 2\"\n"
            "- - 5\n"
            "  - -5\n"
            "- ~\n"
            "-\n"
            "  !tag - TRUE\n"
            "- FALSE\n",

            "- \"a: 2\"\n"
            "- - 5\n"
            "  - -5\n"
            "- ~\n"
            "- !tag\n"
            "  - true\n"
            "- false"
        ));

        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_SEQ, 1, 1, 7, 8));
        Z_ASSERT_EQ(data.seq->datas.len, 5);

        /* "a: 2" */
        elem = data.seq->datas.tab[0];
        Z_HELPER_RUN(z_check_yaml_scalar(&elem, YAML_SCALAR_STRING,
                                         1, 3, 1, 9));
        Z_ASSERT_LSTREQUAL(elem.scalar.s, LSTR("a: 2"));

        /* subseq */
        elem = data.seq->datas.tab[1];
        Z_HELPER_RUN(z_check_yaml_data(&elem, YAML_DATA_SEQ, 2, 3, 3, 7));
        Z_ASSERT_EQ(elem.seq->datas.len, 2);
        Z_HELPER_RUN(z_check_yaml_scalar(&elem.seq->datas.tab[0], YAML_SCALAR_UINT,
                                         2, 5, 2, 6));
        Z_ASSERT_EQ(elem.seq->datas.tab[0].scalar.u, 5UL);
        Z_HELPER_RUN(z_check_yaml_scalar(&elem.seq->datas.tab[1], YAML_SCALAR_INT,
                                         3, 5, 3, 7));
        Z_ASSERT_EQ(elem.seq->datas.tab[1].scalar.i, -5L);

        /* null */
        elem = data.seq->datas.tab[2];
        Z_HELPER_RUN(z_check_yaml_scalar(&elem, YAML_SCALAR_NULL,
                                         4, 3, 4, 4));

        /* subseq */
        elem = data.seq->datas.tab[3];
        Z_HELPER_RUN(z_check_yaml_data(&elem, YAML_DATA_SEQ, 6, 3, 6, 14));
        Z_ASSERT_LSTREQUAL(elem.tag, LSTR("tag"));
        Z_ASSERT_EQ(elem.seq->datas.len, 1);
        Z_HELPER_RUN(z_check_yaml_scalar(&elem.seq->datas.tab[0], YAML_SCALAR_BOOL,
                                         6, 10, 6, 14));
        Z_ASSERT(elem.seq->datas.tab[0].scalar.b);

        /* false */
        elem = data.seq->datas.tab[4];
        Z_HELPER_RUN(z_check_yaml_scalar(&elem, YAML_SCALAR_BOOL,
                                         7, 3, 7, 8));
        Z_ASSERT(!elem.scalar.b);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Parsing complex data */

    Z_TEST(parsing_complex_data, "test parsing of more complex data") {
        t_scope;
        yaml_data_t data;
        yaml_data_t field;

        /* sequence on same level as key */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "a:\n"
            "- 3\n"
            "- ~",

            "a:\n"
            "  - 3\n"
            "  - ~"
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_OBJ, 1, 1, 3, 4));
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT(data.obj->fields.len == 1);
        Z_ASSERT_LSTREQUAL(data.obj->fields.tab[0].key, LSTR("a"));
        field = data.obj->fields.tab[0].data;

        Z_HELPER_RUN(z_check_yaml_data(&field, YAML_DATA_SEQ, 2, 1, 3, 4));
        Z_ASSERT_EQ(field.seq->datas.len, 2);
        Z_HELPER_RUN(z_check_yaml_scalar(&field.seq->datas.tab[0], YAML_SCALAR_UINT,
                                         2, 3, 2, 4));
        Z_ASSERT_EQ(field.seq->datas.tab[0].scalar.u, 3UL);
        Z_HELPER_RUN(z_check_yaml_scalar(&field.seq->datas.tab[1], YAML_SCALAR_NULL,
                                         3, 3, 3, 4));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Parsing flow sequence */

    Z_TEST(parsing_flow_seq, "test parsing of flow sequences") {
        t_scope;
        yaml_data_t data;
        const yaml_data_t *subdata;
        const yaml_data_t *elem;

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "[]",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_SEQ, 1, 1, 1, 3));
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT(data.seq->datas.len == 0);

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "[ ~ ]",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_SEQ, 1, 1, 1, 6));
        Z_ASSERT(data.seq->datas.len == 1);
        elem = &data.seq->datas.tab[0];
        Z_HELPER_RUN(z_check_yaml_scalar(elem, YAML_SCALAR_NULL,
                                         1, 3, 1, 4));

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "[ ~, ]",
            "[ ~ ]"
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_SEQ, 1, 1, 1, 7));
        Z_ASSERT(data.seq->datas.len == 1);
        elem = &data.seq->datas.tab[0];
        Z_HELPER_RUN(z_check_yaml_scalar(elem, YAML_SCALAR_NULL,
                                         1, 3, 1, 4));

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "[1 ,a:\n"
            "2,c d ,]",

            "[ 1, a: 2, c d ]"
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_SEQ, 1, 1, 2, 9));
        Z_ASSERT(data.seq->datas.len == 3);

        elem = &data.seq->datas.tab[0];
        Z_HELPER_RUN(z_check_yaml_scalar(elem, YAML_SCALAR_UINT,
                                         1, 2, 1, 3));
        Z_ASSERT_EQ(elem->scalar.u, 1UL);

        elem = &data.seq->datas.tab[1];
        Z_HELPER_RUN(z_check_yaml_data(elem, YAML_DATA_OBJ, 1, 5, 2, 2));
        Z_ASSERT_EQ(elem->obj->fields.len, 1);
        Z_ASSERT_LSTREQUAL(elem->obj->fields.tab[0].key, LSTR("a"));
        Z_HELPER_RUN(z_check_yaml_span(&elem->obj->fields.tab[0].key_span,
                                       1, 5, 1, 6));
        Z_HELPER_RUN(z_check_yaml_scalar(&elem->obj->fields.tab[0].data,
                                         YAML_SCALAR_UINT, 2, 1, 2, 2));
        Z_ASSERT_EQ(elem->obj->fields.tab[0].data.scalar.u, 2UL);

        elem = &data.seq->datas.tab[2];
        Z_HELPER_RUN(z_check_yaml_scalar(elem, YAML_SCALAR_STRING,
                                         2, 3, 2, 6));
        Z_ASSERT_LSTREQUAL(elem->scalar.s, LSTR("c d"));

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "- [ ~,\n"
            " [[ true, [ - 2 ] ]\n"
            "   ] , a:  [  -2] ,\n"
            "]",

            "- [ ~, [ [ true, [ \"- 2\" ] ] ], a: [ -2 ] ]"
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_SEQ, 1, 1, 4, 2));
        Z_ASSERT(data.seq->datas.len == 1);
        data = data.seq->datas.tab[0];

        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_SEQ, 1, 3, 4, 2));
        Z_ASSERT(data.seq->datas.len == 3);
        /* first elem: ~ */
        elem = &data.seq->datas.tab[0];
        Z_HELPER_RUN(z_check_yaml_scalar(elem, YAML_SCALAR_NULL, 1, 5, 1, 6));
        /* second elem: [[ true, [-2]] */
        elem = &data.seq->datas.tab[1];
        Z_HELPER_RUN(z_check_yaml_data(elem, YAML_DATA_SEQ, 2, 2, 3, 5));
        Z_ASSERT(elem->seq->datas.len == 1);

        /* [ true, [-2] ] */
        subdata = &elem->seq->datas.tab[0];
        Z_HELPER_RUN(z_check_yaml_data(subdata, YAML_DATA_SEQ, 2, 3, 2, 20));
        Z_ASSERT(subdata->seq->datas.len == 2);
        elem = &subdata->seq->datas.tab[0];
        Z_HELPER_RUN(z_check_yaml_scalar(elem, YAML_SCALAR_BOOL, 2, 5, 2, 9));
        elem = &subdata->seq->datas.tab[1];
        Z_HELPER_RUN(z_check_yaml_data(elem, YAML_DATA_SEQ, 2, 11, 2, 18));
        Z_ASSERT_EQ(elem->seq->datas.len, 1);
        Z_HELPER_RUN(z_check_yaml_scalar(&elem->seq->datas.tab[0],
                                         YAML_SCALAR_STRING, 2, 13, 2, 16));
        Z_ASSERT_LSTREQUAL(elem->seq->datas.tab[0].scalar.s, LSTR("- 2"));

        /* third elem: a: [-2] */
        elem = &data.seq->datas.tab[2];
        Z_HELPER_RUN(z_check_yaml_data(elem, YAML_DATA_OBJ, 3, 8, 3, 18));
        Z_ASSERT_EQ(elem->obj->fields.len, 1);
        /* [-2] */
        Z_ASSERT_LSTREQUAL(elem->obj->fields.tab[0].key, LSTR("a"));
        Z_HELPER_RUN(z_check_yaml_span(&elem->obj->fields.tab[0].key_span,
                                       3, 8, 3, 9));
        subdata = &elem->obj->fields.tab[0].data;
        Z_HELPER_RUN(z_check_yaml_data(subdata, YAML_DATA_SEQ, 3, 12, 3, 18));
        Z_ASSERT_EQ(subdata->seq->datas.len, 1);
        Z_HELPER_RUN(z_check_yaml_scalar(&subdata->seq->datas.tab[0],
                                         YAML_SCALAR_INT, 3, 15, 3, 17));
        Z_ASSERT_EQ(subdata->seq->datas.tab[0].scalar.i, -2L);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Parsing flow object */

    Z_TEST(parsing_flow_obj, "test parsing of flow objects") {
        t_scope;
        yaml_data_t data;
        const yaml_key_data_t *elem;

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "{}",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_OBJ, 1, 1, 1, 3));
        Z_ASSERT_NULL(data.tag.s);
        Z_ASSERT(data.obj->fields.len == 0);

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "{ a: ~ }",
            NULL
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_OBJ, 1, 1, 1, 9));
        Z_ASSERT(data.obj->fields.len == 1);
        elem = &data.obj->fields.tab[0];
        Z_ASSERT_LSTREQUAL(elem->key, LSTR("a"));
        Z_HELPER_RUN(z_check_yaml_span(&elem->key_span, 1, 3, 1, 4));
        Z_HELPER_RUN(z_check_yaml_scalar(&elem->data, YAML_SCALAR_NULL,
                                         1, 6, 1, 7));

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "{ a: foo, }",
            "{ a: foo }"
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_OBJ, 1, 1, 1, 12));
        Z_ASSERT(data.obj->fields.len == 1);
        elem = &data.obj->fields.tab[0];
        Z_ASSERT_LSTREQUAL(elem->key, LSTR("a"));
        Z_HELPER_RUN(z_check_yaml_span(&elem->key_span, 1, 3, 1, 4));
        Z_HELPER_RUN(z_check_yaml_scalar(&elem->data, YAML_SCALAR_STRING,
                                         1, 6, 1, 9));
        Z_ASSERT_LSTREQUAL(elem->data.scalar.s, LSTR("foo"));

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "{ a: ~ ,b:\n"
            "2,}",

            "{ a: ~, b: 2 }"
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_OBJ, 1, 1, 2, 4));
        Z_ASSERT(data.obj->fields.len == 2);
        elem = &data.obj->fields.tab[0];
        Z_ASSERT_LSTREQUAL(elem->key, LSTR("a"));
        Z_HELPER_RUN(z_check_yaml_span(&elem->key_span, 1, 3, 1, 4));
        Z_HELPER_RUN(z_check_yaml_scalar(&elem->data, YAML_SCALAR_NULL,
                                         1, 6, 1, 7));
        elem = &data.obj->fields.tab[1];
        Z_ASSERT_LSTREQUAL(elem->key, LSTR("b"));
        Z_HELPER_RUN(z_check_yaml_span(&elem->key_span, 1, 9, 1, 10));
        Z_HELPER_RUN(z_check_yaml_scalar(&elem->data, YAML_SCALAR_UINT,
                                         2, 1, 2, 2));
        Z_ASSERT_EQ(elem->data.scalar.u, 2UL);

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "- { a: [true,\n"
            "   false,]\n"
            "     , b: f   \n"
            "  ,\n"
            "    z: { y: 1  }}\n"
            "- ~",

            "- { a: [ true, false ], b: f, z: { y: 1 } }\n"
            "- ~"
        ));
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_SEQ, 1, 1, 6, 4));
        Z_ASSERT(data.seq->datas.len == 2);
        Z_HELPER_RUN(z_check_yaml_scalar(&data.seq->datas.tab[1],
                                         YAML_SCALAR_NULL, 6, 3, 6, 4));

        data = data.seq->datas.tab[0];
        Z_HELPER_RUN(z_check_yaml_data(&data, YAML_DATA_OBJ, 1, 3, 5, 18));
        Z_ASSERT(data.seq->datas.len == 3);

        elem = &data.obj->fields.tab[0];
        Z_ASSERT_LSTREQUAL(elem->key, LSTR("a"));
        Z_HELPER_RUN(z_check_yaml_span(&elem->key_span, 1, 5, 1, 6));
        Z_HELPER_RUN(z_check_yaml_data(&elem->data, YAML_DATA_SEQ,
                                         1, 8, 2, 11));
        Z_ASSERT_EQ(elem->data.seq->datas.len,2);
        Z_HELPER_RUN(z_check_yaml_scalar(&elem->data.seq->datas.tab[0],
                                         YAML_SCALAR_BOOL, 1, 9, 1, 13));
        Z_ASSERT(elem->data.seq->datas.tab[0].scalar.b);
        Z_HELPER_RUN(z_check_yaml_scalar(&elem->data.seq->datas.tab[1],
                                         YAML_SCALAR_BOOL, 2, 4, 2, 9));
        Z_ASSERT(!elem->data.seq->datas.tab[1].scalar.b);

        elem = &data.obj->fields.tab[1];
        Z_ASSERT_LSTREQUAL(elem->key, LSTR("b"));
        Z_HELPER_RUN(z_check_yaml_span(&elem->key_span, 3, 8, 3, 9));
        Z_HELPER_RUN(z_check_yaml_scalar(&elem->data, YAML_SCALAR_STRING,
                                         3, 11, 3, 12));
        Z_ASSERT_LSTREQUAL(elem->data.scalar.s, LSTR("f"));

        elem = &data.obj->fields.tab[2];
        Z_ASSERT_LSTREQUAL(elem->key, LSTR("z"));
        Z_HELPER_RUN(z_check_yaml_span(&elem->key_span, 5, 5, 5, 6));
        Z_HELPER_RUN(z_check_yaml_data(&elem->data, YAML_DATA_OBJ,
                                       5, 8, 5, 17));
        Z_ASSERT_EQ(elem->data.obj->fields.len, 1);

        /* { y: 1 } */
        elem = &elem->data.obj->fields.tab[0];
        Z_ASSERT_LSTREQUAL(elem->key, LSTR("y"));
        Z_HELPER_RUN(z_check_yaml_span(&elem->key_span, 5, 10, 5, 11));
        Z_HELPER_RUN(z_check_yaml_scalar(&elem->data, YAML_SCALAR_UINT,
                                         5, 13, 5, 14));
        Z_ASSERT_EQ(elem->data.scalar.u, 1UL);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Packing simple data */

    Z_TEST(pack, "test packing of simple data") {
        t_scope;
        yaml_data_t scalar;
        yaml_data_t data;
        yaml_data_t data2;

        /* empty obj */
        t_yaml_data_new_obj(&data, 0);
        Z_HELPER_RUN(z_check_yaml_pack(&data, NULL, "{}"));

        /* empty obj in seq */
        t_yaml_data_new_seq(&data2, 1);
        yaml_seq_add_data(&data2, data);
        Z_HELPER_RUN(z_check_yaml_pack(&data2, NULL, "- {}"));

        /* empty seq */
        t_yaml_data_new_seq(&data, 0);
        Z_HELPER_RUN(z_check_yaml_pack(&data, NULL, "[]"));

        /* empty seq in obj */
        t_yaml_data_new_obj(&data2, 1);
        yaml_obj_add_field(&data2, LSTR("a"), data);
        Z_HELPER_RUN(z_check_yaml_pack(&data2, NULL, "a: []"));

        /* seq in seq */
        t_yaml_data_new_seq(&data, 1);
        yaml_data_set_bool(&scalar, true);
        yaml_seq_add_data(&data, scalar);
        t_yaml_data_new_seq(&data2, 1);
        yaml_seq_add_data(&data2, data);
        Z_HELPER_RUN(z_check_yaml_pack(&data2, NULL, "- - true"));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Packing flags */

    Z_TEST(pack_flags, "test packing flags") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t pres;
        yaml_parse_t *env;

        Z_HELPER_RUN(z_write_yaml_file("not_recreated.yml", "1"));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            "key: !include not_recreated.yml",

            "key: 1"
        ));

        Z_HELPER_RUN(z_create_tmp_subdir("flags"));
        Z_HELPER_RUN(z_pack_yaml_file("flags/root.yml", &data, &pres,
                                      YAML_PACK_NO_SUBFILES));
        Z_HELPER_RUN(z_check_file("flags/root.yml",
            "key: !include not_recreated.yml\n"
        ));
        Z_HELPER_RUN(z_check_file_do_not_exist("flags/not_recreated.yml"));
        yaml_parse_delete(&env);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Comment presentation */

#define CHECK_PREFIX_COMMENTS(pres, path, ...)                               \
    do {                                                                     \
        lstr_t comments[] = { __VA_ARGS__ };                                 \
                                                                             \
        Z_HELPER_RUN(z_check_prefix_comments((pres), (path), comments,       \
                                             countof(comments)));            \
    } while (0)

    Z_TEST(comment_presentation, "test saving of comments in presentation") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t doc_pres;
        const yaml_presentation_t *pres;

        /* comment on a scalar => not saved */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &doc_pres, NULL,
            "# my scalar\n"
            "3",
            NULL
        ));
        pres = t_yaml_doc_pres_to_map(&doc_pres);
        Z_ASSERT_P(pres);
        Z_ASSERT_EQ(1, qm_len(yaml_pres_node, &pres->nodes));
        CHECK_PREFIX_COMMENTS(pres, LSTR("!"), LSTR("my scalar"));

        /* comment on a key => path is key */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &doc_pres, NULL,
            "a: 3 #ticket is #42  ",

            "a: 3 # ticket is #42\n"
        ));
        pres = t_yaml_doc_pres_to_map(&doc_pres);
        Z_ASSERT_P(pres);
        Z_ASSERT_EQ(1, qm_len(yaml_pres_node, &pres->nodes));
        Z_HELPER_RUN(z_check_inline_comment(pres, LSTR(".a!"),
                                            LSTR("ticket is #42")));

        /* comment on a list => path is index */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &doc_pres, NULL,
            "# prefix comment\n"
            "- 1 # first\n"
            "- # item\n"
            "  2 # second\n",

            NULL
        ));
        pres = t_yaml_doc_pres_to_map(&doc_pres);
        Z_ASSERT_P(pres);
        Z_ASSERT_EQ(4, qm_len(yaml_pres_node, &pres->nodes));
        CHECK_PREFIX_COMMENTS(pres, LSTR("!"), LSTR("prefix comment"));
        Z_HELPER_RUN(z_check_inline_comment(pres, LSTR("[0]!"),
                                            LSTR("first")));
        Z_HELPER_RUN(z_check_inline_comment(pres, LSTR("[1]"),
                                            LSTR("item")));
        Z_HELPER_RUN(z_check_inline_comment(pres, LSTR("[1]!"),
                                            LSTR("second")));

        /* prefix comment with multiple lines */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &doc_pres, NULL,
            "key:\n"
            "   # first line\n"
            " # and second\n"
            "     # bad indent is ok\n"
            "  a: # inline a\n"
            " # prefix scalar\n"
            "     ~ # inline scalar\n"
            "    # this is lost",

            "key:\n"
            "  # first line\n"
            "  # and second\n"
            "  # bad indent is ok\n"
            "  a: # inline a\n"
            "    # prefix scalar\n"
            "    ~ # inline scalar\n"
        ));
        pres = t_yaml_doc_pres_to_map(&doc_pres);
        Z_ASSERT_P(pres);
        Z_ASSERT_EQ(3, qm_len(yaml_pres_node, &pres->nodes));
        CHECK_PREFIX_COMMENTS(pres, LSTR(".key!"),
                              LSTR("first line"),
                              LSTR("and second"),
                              LSTR("bad indent is ok"));
        Z_HELPER_RUN(z_check_inline_comment(pres, LSTR(".key.a"),
                                            LSTR("inline a")));
        CHECK_PREFIX_COMMENTS(pres, LSTR(".key.a!"), LSTR("prefix scalar"));
        Z_HELPER_RUN(z_check_inline_comment(pres, LSTR(".key.a!"),
                                            LSTR("inline scalar")));

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &doc_pres, NULL,
            "# prefix key\n"
            "key: # inline key\n"
            "# prefix [0]\n"
            "- # inline [0]\n"
            " # prefix key2\n"
            " key2: ~ # inline key2\n",

            "# prefix key\n"
            "key: # inline key\n"
            "  # prefix [0]\n"
            "  - # inline [0]\n"
            "    # prefix key2\n"
            "    key2: ~ # inline key2\n"
        ));
        pres = t_yaml_doc_pres_to_map(&doc_pres);
        Z_ASSERT_P(pres);
        Z_ASSERT_EQ(6, qm_len(yaml_pres_node, &pres->nodes));
        CHECK_PREFIX_COMMENTS(pres, LSTR("!"),
                              LSTR("prefix key"));
        Z_HELPER_RUN(z_check_inline_comment(pres, LSTR(".key"),
                                            LSTR("inline key")));
        CHECK_PREFIX_COMMENTS(pres, LSTR(".key!"),
                              LSTR("prefix [0]"));
        Z_HELPER_RUN(z_check_inline_comment(pres, LSTR(".key[0]"),
                                            LSTR("inline [0]")));
        CHECK_PREFIX_COMMENTS(pres, LSTR(".key[0]!"),
                              LSTR("prefix key2"));
        Z_HELPER_RUN(z_check_inline_comment(pres, LSTR(".key[0].key2!"),
                                            LSTR("inline key2")));

        /* prefix comment must be written before tag */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "# prefix key\n"
            "!toto 3",

            NULL
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "# a\n"
            "a: # b\n"
            "  !foo b",

            NULL
        ));

        /* inline comments with flow data */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "- # prefix\n"
            "  1 # inline\n",
            NULL
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, NULL, NULL,
            "- # prefix\n"
            "  [ 1 ] # inline\n"
            "- # prefix2\n"
            "  { a: b } # inline2\n",

            NULL
        ));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Empty lines presentation */

    Z_TEST(empty_lines_presentation, "") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t pres;

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, NULL,
            "\n"
            "  # comment\n"
            "\n"
            "a: ~",

            /* First empty lines, then prefix comment. */
            "\n"
            "\n"
            "# comment\n"
            "a: ~"
        ));

        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, NULL,
            "# 1\n"
            "a: # 2\n"
            "\n"
            "  - b: 3\n"
            "\n"
            "    c: 4\n"
            "\n"
            "  -\n"
            "\n"
            "    # foo\n"
            "    2\n"
            "  - 3",

            NULL
        ));

        /* max two new lines */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, NULL,
            "\n\n\n\n"
            "a: 4\n"
            "\n\n\n"
            "b: 3\n"
            "\n"
            "# comment\n"
            "\n"
            "c: 2\n"
            "\n"
            "d: 1\n"
            "e: 0",

            "\n\n"
            "a: 4\n"
            "\n\n"
            "b: 3\n"
            "\n\n"
            "# comment\n"
            "c: 2\n"
            "\n"
            "d: 1\n"
            "e: 0"
        ));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Flow presentation */

    Z_TEST(flow_presentation, "") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t pres;
        const char *expected;

        /* Make sure that flow syntax is reverted if a tag is added in the
         * data. */
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, NULL,
            "a: { k: d }\n"
            "b: [ 1, 2 ]",

            NULL
        ));
        data.obj->fields.tab[0].data.obj->fields.tab[0].data.tag
            = LSTR("tag1");
        data.obj->fields.tab[1].data.seq->datas.tab[1].tag = LSTR("tag2");

        expected =
            "a:\n"
            "  k: !tag1 d\n"
            "b:\n"
            "  - 1\n"
            "  - !tag2 2";
        Z_HELPER_RUN(z_check_yaml_pack(&data, NULL, expected));
        Z_HELPER_RUN(z_check_yaml_pack(&data, &pres, expected));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Variables */

    Z_TEST(variables, "") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t pres;
        yaml_parse_t *env;
        const char *inner;
        const char *root;

        /* test replacement of variables */
        inner =
            "- a:\n"
            "    - 1\n"
            "    - $a\n"
            "- b:\n"
            "    a: $a\n"
            "    b: $ab\n";
        Z_HELPER_RUN(z_write_yaml_file("inner.yml", inner));
        root =
            "!include inner.yml\n"
            "$a: 3\n"
            "$ab:\n"
            "  - 1\n"
            "  - 2\n";
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            root,

            "- a:\n"
            "    - 1\n"
            "    - 3\n"
            "- b:\n"
            "    a: 3\n"
            "    b:\n"
            "      - 1\n"
            "      - 2"
        ));

        /* pack into files, to test repacking of variables */
        Z_HELPER_RUN(z_pack_yaml_file("variables_1/root.yml", &data, &pres,
                                      0));
        Z_HELPER_RUN(z_check_file("variables_1/root.yml", root));
        Z_HELPER_RUN(z_check_file("variables_1/inner.yml", inner));
        yaml_parse_delete(&env);

        /* test combination of variables settings + override */
        Z_HELPER_RUN(z_write_yaml_file("inner.yml",
            "var: $var\n"
            "a: 0\n"
            "b: 1"
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            "- !include inner.yml\n"
            "  $var: 3\n"
            "  b: 4",

            "- var: 3\n"
            "  a: 0\n"
            "  b: 4"
        ));
        yaml_parse_delete(&env);
    } Z_TEST_END;

    /* }}} */
    /* {{{ Variables in strings */

    Z_TEST(variables_in_strings, "") {
        t_scope;
        yaml_data_t data;
        yaml__document_presentation__t pres;
        yaml_parse_t *env;

        /* test replacement of variables */
        /* TODO: handle variables in flow context? */
        Z_HELPER_RUN(z_write_yaml_file("inner.yml",
            "- \"foo var is: `$foo`\"\n"
            "- <$foo> unquoted also works </$foo>\n"
            "- a: $foo\n"
            "  b: $foo-$foo-$qux-$foo"
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            "!include inner.yml\n"
            "$foo: bar\n"
            "$qux: c",

            "- \"foo var is: `bar`\"\n"
            "- <bar> unquoted also works </bar>\n"
            "- a: bar\n"
            "  b: bar-bar-c-bar"
        ));
        yaml_parse_delete(&env);

        /* test partial modification of templated string */
        Z_HELPER_RUN(z_write_yaml_file("grandchild.yml",
            "addr: \"$host:$port\""
        ));
        Z_HELPER_RUN(z_write_yaml_file("child.yml",
            "!include grandchild.yml\n"
            "$port: 80"
        ));
        Z_HELPER_RUN(z_t_yaml_test_parse_success(&data, &pres, &env,
            "!include child.yml\n"
            "$host: website.org",

            "addr: \"website.org:80\""
        ));
    } Z_TEST_END;

    /* }}} */
    /* {{{ Variable errors */

    Z_TEST(variable_errors, "") {
        t_scope;

        Z_HELPER_RUN(z_write_yaml_file("inner.yml",
            "a: $a\n"
            "s: \"<$s>\"\n"
            "t: <$t>"
        ));

        /* unknown variable being set */
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "key: !include inner.yml\n"
            "  $b: foo",

            "input.yml:2:3: invalid key, unknown variable\n"
            "  $b: foo\n"
            "  ^^"
        ));

        /* string-variable being set with wrong type */
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "key: !include inner.yml\n"
            "  $s: [ 1, 2 ]",

            "input.yml:2:7: wrong type of data, "
            "this variable can only be set with a scalar\n"
            "  $s: [ 1, 2 ]\n"
            "      ^^^^^^^^"
        ));
        Z_HELPER_RUN(z_yaml_test_file_parse_fail(
            "key: !include inner.yml\n"
            "  $t: [ 1, 2 ]",

            "input.yml:2:7: wrong type of data, "
            "this variable can only be set with a scalar\n"
            "  $t: [ 1, 2 ]\n"
            "      ^^^^^^^^"
        ));
    } Z_TEST_END;

    /* }}} */

    MODULE_RELEASE(yaml);
} Z_GROUP_END

/* LCOV_EXCL_STOP */

/* }}} */

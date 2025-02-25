#include <stdbool.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <rime_api.h>

#include <unistd.h>

#include "interface.h"
#include "liberime.h"

/**
 * Macro that defines a docstring for a function.
 * @param name The function name (without liberime_ prefix).
 * @param args The argument list as visible from Emacs (without parens).
 * @param docstring The rest of the documentation.
 */
#define DOCSTRING(name, args, docstring)                                \
const char *liberime_##name##__doc = (docstring "\n\n(fn " args ")")

#define DEFUN(ename, cname, min_nargs, max_nargs)                       \
  em_defun(env, (ename),                                                \
           env->make_function(env, (min_nargs), (max_nargs),            \
                              cname,                                    \
                              liberime_##cname##__doc,                  \
                              rime))

#define CONS_INT(key, integer)                                          \
  em_cons(env, env->intern(env, key), env->make_integer(env, integer));
#define CONS_STRING(key, str)                                           \
  em_cons(env, env->intern(env, key), env->make_string(env, str, strlen(str)))
#define CONS_VALUE(key, value) \
  em_cons(env, env->intern(env, key), value)

#define CANDIDATE_MAXSTRLEN 1024
#define SCHEMA_MAXSTRLEN 1024

#define NO_SESSION_ERR "Cannot connect to librime session, make sure to run liberime-start first"

typedef struct _EmacsRime {
  RimeSessionId session_id;
  RimeApi *api;
  bool firstRun;
  emacs_env *EmacsEnv;
} EmacsRime;

typedef struct _CandidateLinkedList {
  char *value;
  struct _CandidateLinkedList *next;
} CandidateLinkedList;

typedef struct _EmacsRimeCandidates {
  size_t size;
  CandidateLinkedList *list;
} EmacsRimeCandidates;

void notification_handler(void *context,
                          RimeSessionId session_id,
                          const char *message_type,
                          const char *message_value) {
  EmacsRime *rime = (EmacsRime*) context;
  emacs_env *env = rime->EmacsEnv;
  char format[] = "[liberime] %s: %s";
  emacs_value args[3];
  args[0] = env->make_string(env, format, strnlen(format, SCHEMA_MAXSTRLEN));
  args[1] = env->make_string(env, message_type, strnlen(message_type, SCHEMA_MAXSTRLEN));
  args[2] = env->make_string(env, message_value, strnlen(message_value, SCHEMA_MAXSTRLEN));
  env->funcall(env, env->intern (env, "message"), 3, args);
}

// unused for now
static bool _ensure_session(EmacsRime *rime) {
  if (!rime->api->find_session(rime->session_id)) {
    rime->session_id = rime->api->create_session();
    if (!rime->session_id) {
      // printf("cannot create rime session\n");
      return false;
    }
  }
  return true;
}

static char *_copy_string(char *string) {
  size_t size = strnlen(string, CANDIDATE_MAXSTRLEN);
  char *new_str = malloc(size+1);
  strncpy(new_str, string, size);
  new_str[size] = '\0';
  return new_str;
}

EmacsRimeCandidates _get_candidates(EmacsRime *rime, size_t limit) {
  EmacsRimeCandidates c = {.size=0, .list=(CandidateLinkedList *)malloc(sizeof(CandidateLinkedList))};

  RimeCandidateListIterator iterator = {0};
  CandidateLinkedList *next = c.list;
  if (rime->api->candidate_list_begin(rime->session_id, &iterator)) {
    while (rime->api->candidate_list_next(&iterator) && (limit == 0 || c.size < limit)) {
      c.size += 1;

      next->value = _copy_string(iterator.candidate.text);
      next->next = (CandidateLinkedList *)malloc(sizeof(CandidateLinkedList));

      next = next->next;
    }
    next->next = NULL;
    rime->api->candidate_list_end(&iterator);
  }

  return c;
}

// bindings
DOCSTRING(start, "SHARED_DATA_DIR USER_DATA_DIR",
          "Start a rime session");
static emacs_value start(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;

  char *shared_data_dir = em_get_string(env, em_expand_file_name(env, args[0]));
  char *user_data_dir = em_get_string(env, em_expand_file_name(env, args[1]));

  RIME_STRUCT(RimeTraits, emacs_rime_traits);

  emacs_rime_traits.shared_data_dir = shared_data_dir;
  emacs_rime_traits.app_name = "rime.emacs";
  emacs_rime_traits.user_data_dir = user_data_dir;
  emacs_rime_traits.distribution_name = "Rime";
  emacs_rime_traits.distribution_code_name = "emacs-rime";
  emacs_rime_traits.distribution_version = "0.1.0";
  if (rime->firstRun) {
    rime->api->setup(&emacs_rime_traits);
    rime->firstRun = false;
  }

  rime->api->initialize(&emacs_rime_traits);
  // Let notification_handler can access emacs_env
  rime->EmacsEnv = env;
  rime->api->set_notification_handler(notification_handler, rime);
  rime->api->start_maintenance(true);

  // wait for deploy
  rime->api->join_maintenance_thread();

  rime->session_id = rime->api->create_session();

  return em_t;
}

DOCSTRING(finalize, "", "finalize librime for redeploy");
static emacs_value finalize(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;
  if (rime->session_id) {
    rime->api->sync_user_data();
    rime->session_id = 0;
  }
  rime->api->finalize();
  return em_t;
}

void free_candidate_list(CandidateLinkedList *list) {
  CandidateLinkedList *next = list;
  while (next) {
    CandidateLinkedList *temp = next;
    next = temp->next;
    // do not free temp->value
    // it seems emacs_env->make_string didn't do copy
    /* if (temp->value) { */
    /*    free(temp->value); */
    /* } */
    free(temp);
  }
}

DOCSTRING(get_sync_dir, "", "get sync dir");
static emacs_value get_sync_dir(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;
  if (!_ensure_session(rime)) {
    em_signal_rimeerr(env, 1, NO_SESSION_ERR);
    return em_nil;
  }

  const char *sync_dir = rime->api->get_sync_dir();
  return env->make_string(env, sync_dir, strlen(sync_dir));
}

DOCSTRING(sync_user_data, "", "Sync user data");
static emacs_value sync_user_data(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;
  if (!_ensure_session(rime)) {
    em_signal_rimeerr(env, 1, NO_SESSION_ERR);
    return em_nil;
  }

  bool result = rime->api->sync_user_data();
  return result ? em_t : em_nil;
}

DOCSTRING(get_schema_list, "", "List all rime schema.");
static emacs_value get_schema_list(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;
  if (!_ensure_session(rime)) {
    em_signal_rimeerr(env, 1, NO_SESSION_ERR);
    return em_nil;
  }

  RimeSchemaList schema_list;
  if (!rime->api->get_schema_list(&schema_list)) {
    em_signal_rimeerr(env, 1, "Get schema list form librime failed");
    return em_nil;
  }

  emacs_value flist = env->intern(env, "list");
  emacs_value array[schema_list.size];
  for (int i = 0; i < schema_list.size; i++) {
    RimeSchemaListItem item = schema_list.list[i];
    emacs_value pair[2];
    pair[0] = env->make_string(env, item.schema_id, strnlen(item.schema_id, SCHEMA_MAXSTRLEN));
    pair[1] = env->make_string(env, item.name, strnlen(item.name, SCHEMA_MAXSTRLEN));

    array[i] = env->funcall(env, flist, 2, pair);
  }

  emacs_value result = env->funcall(env, flist, schema_list.size, array);

  rime->api->free_schema_list(&schema_list);

  return result;
}

DOCSTRING(select_schema, "SCHEMA_ID",
            "Select a rime schema.\n"
            "SCHENA_ID should be a value returned from `liberime-get-schema-list'.");
static emacs_value select_schema(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;
  const char *schema_id = em_get_string(env, args[0]);
  if (!_ensure_session(rime)) {
    em_signal_rimeerr(env, 1, NO_SESSION_ERR);
    return em_nil;
  }

  if (rime->api->select_schema(rime->session_id, schema_id)) {
    return em_t;
  }
  return em_nil;
}

// input
DOCSTRING(process_key, "KEY", "process key");
static emacs_value process_key(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;

  int keycode = env->extract_integer(env, args[0]);
  // printf("keycode is %d\n", keycode);
  // const char *key = em_get_string(env, args[0]);

  if (!_ensure_session(rime)) {
    em_signal_rimeerr(env, 1, NO_SESSION_ERR);
    return em_nil;
  }

  if (rime->api->process_key(rime->session_id, keycode, 0)) {
    return em_t;
  }
  return em_nil;
}

DOCSTRING(commit_composition, "", "Commit");
static emacs_value commit_composition(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;

  if (!_ensure_session(rime)) {
    em_signal_rimeerr(env, 1, NO_SESSION_ERR);
    return em_nil;
  }

  if (rime->api->commit_composition(rime->session_id)) {
    return em_t;
  }
  return em_nil;
}

DOCSTRING(clear_composition, "", "Clear");
static emacs_value clear_composition(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;

  if (!_ensure_session(rime)) {
    em_signal_rimeerr(env, 1, NO_SESSION_ERR);
    return em_nil;
  }

  rime->api->clear_composition(rime->session_id);
  return em_t;
}

DOCSTRING(select_candidate, "NUM", "Select");
static emacs_value select_candidate(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;

  int index = env->extract_integer(env, args[0]);

  if (rime->api->select_candidate_on_current_page(rime->session_id, index)) {
    return em_t;
  }
  return em_nil;
}

// output

DOCSTRING(get_commit, "", "Get commit");
static emacs_value get_commit(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;

  if (!_ensure_session(rime)) {
    em_signal_rimeerr(env, 1, NO_SESSION_ERR);
    return em_nil;
  }

  RIME_STRUCT(RimeCommit, commit);
  if (rime->api->get_commit(rime->session_id, &commit)) {
    if (!commit.text) {
      return em_nil;
    }

    char *commit_str = _copy_string(commit.text);
    rime->api->free_commit(&commit);
    // printf("commit str is %s\n", commit_str);

    return env->make_string(env, commit_str, strlen(commit_str));
  }

  return em_nil;
}

DOCSTRING(get_context, "", "Get context");
static emacs_value get_context(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
  EmacsRime *rime = (EmacsRime*) data;

  if (!_ensure_session(rime)) {
    em_signal_rimeerr(env, 1, NO_SESSION_ERR);
    return em_nil;
  }

  RIME_STRUCT(RimeContext, context);
  if (!rime->api->get_context(rime->session_id, &context)){
    em_signal_rimeerr(env, 2, "cannot get context");
    return em_nil;
  }

  if (!context.menu.num_candidates) {
    return em_nil;
  }

  size_t result_size = 3;
  emacs_value result_a[result_size];

  // 0. context.commit_text_preview
  char *ctp_str = _copy_string(context.commit_text_preview);
  result_a[0] = CONS_STRING("commit-text-preview", ctp_str);

  // 2. context.composition
  emacs_value composition_a[5];
  composition_a[0] = CONS_INT("length", context.composition.length);
  composition_a[1] = CONS_INT("cursor-pos", context.composition.cursor_pos);
  composition_a[2] = CONS_INT("sel-start", context.composition.sel_start);
  composition_a[3] = CONS_INT("sel-end", context.composition.sel_end);

  char *preedit_str = _copy_string(context.composition.preedit);
  composition_a[4] = CONS_STRING("preedit", preedit_str);

  emacs_value composition_value = em_list(env, 5, composition_a);
  result_a[1] = CONS_VALUE("composition", composition_value);

  // 3. context.menu
  emacs_value menu_a[6];
  menu_a[0] = CONS_INT("highlighted-candidate-index", context.menu.highlighted_candidate_index);
  menu_a[1] = CONS_VALUE("last-page-p", context.menu.is_last_page ? em_t : em_nil);
  menu_a[2] = CONS_INT("num-candidates", context.menu.num_candidates);
  menu_a[3] = CONS_INT("page-no", context.menu.page_no);
  menu_a[4] = CONS_INT("page-size", context.menu.page_size);

  emacs_value carray[context.menu.num_candidates];
  for (int i = 0; i < context.menu.num_candidates; i++) {
    RimeCandidate c = context.menu.candidates[i];
    char *ctext = _copy_string(c.text);
    carray[i] = env->make_string(env, ctext, strlen(ctext));
  }
  emacs_value candidates = em_list(env, context.menu.num_candidates, carray);
  menu_a[5] = CONS_VALUE("candidates", candidates);
  emacs_value menu = em_list(env, 6, menu_a);
  result_a[2] = CONS_VALUE("menu", menu);

  // build result
  emacs_value result = em_list(env, result_size, result_a);

  rime->api->free_context(&context);

  return result;
}

void liberime_init(emacs_env *env) {
  // Name 'rime' is hardcode in DEFUN micro, so if you edit here,
  // you should edit DEFUN micro too.
  EmacsRime *rime = (EmacsRime*) malloc(sizeof(EmacsRime));

  rime->api = rime_get_api();
  rime->firstRun = true; // not used yet

  if (!rime->api) {
    free(rime);
    em_signal_rimeerr(env, 1, "No librime found");
    return;
  }
 
  DEFUN("liberime-start", start, 2, 2);
  DEFUN("liberime-select-schema", select_schema, 1, 1);
  DEFUN("liberime-get-schema-list", get_schema_list, 0, 0);

  // input
  DEFUN("liberime-process-key", process_key, 1, 1);
  DEFUN("liberime-commit-composition", commit_composition, 0, 0);
  DEFUN("liberime-clear-composition", clear_composition, 0, 0);
  DEFUN("liberime-select-candidate", select_candidate, 1, 1);

  // output
  DEFUN("liberime-get-commit", get_commit, 0, 0);
  DEFUN("liberime-get-context", get_context, 0, 0);

  // sync
  DEFUN("liberime-get-sync-dir", get_sync_dir, 0, 0);
  DEFUN("liberime-sync-user-data", sync_user_data, 0, 0);
  DEFUN("liberime-finalize", finalize, 0, 0);

}

#ifndef RIME_USER_DICTIONARY_API_H_
#define RIME_USER_DICTIONARY_API_H_

#include <rime_api.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rime_user_dictionary_api_t {
  int data_size;

  // Add an explicit entry to the active schema's first compatible writable
  // table/script user dictionary. The code is normalized by librime before
  // the already-open userdb is updated; no deployment or session rebuild is
  // required.
  Bool (*add_user_entry)(RimeSessionId session_id,
                         const char* text,
                         const char* code);
} RimeUserDictionaryApi;

#ifdef __cplusplus
}
#endif

#endif  // RIME_USER_DICTIONARY_API_H_

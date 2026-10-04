// Copyright RIME Developers
// Distributed under the BSD License

#include <rime_user_dictionary_api.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string/trim.hpp>
#include <rime/algo/syllabifier.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/user_dictionary.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/gear/translator_commons.h>

using namespace rime;

namespace {

constexpr size_t kMaxUserEntryTextBytes = 4096;
constexpr size_t kMaxUserEntryCodeBytes = 512;

bool IsSafeUserDbField(const std::string& value) {
  return value.find('\t') == std::string::npos &&
         value.find('\r') == std::string::npos &&
         value.find('\n') == std::string::npos;
}

struct TranslatorSpec {
  std::string klass;
  std::string name_space = "translator";
};

TranslatorSpec ParseTranslatorSpec(const std::string& prescription) {
  TranslatorSpec spec;
  const auto separator = prescription.find('@');
  if (separator == std::string::npos) {
    spec.klass = prescription;
  } else {
    spec.klass = prescription.substr(0, separator);
    if (separator + 1 < prescription.size())
      spec.name_space = prescription.substr(separator + 1);
  }
  return spec;
}

struct EdgeChoice {
  size_t end = 0;
  SyllableId syllable_id = 0;
  SpellingType type = kInvalidSpelling;
  double credibility = 0.0;
};

bool FindSyllablePath(const SyllableGraph& graph,
                      size_t pos,
                      std::vector<SyllableId>* path,
                      std::vector<int>* memo) {
  if (pos == graph.interpreted_length)
    return true;
  if (pos >= memo->size() || (*memo)[pos] < 0)
    return false;

  const auto edge_it = graph.edges.find(pos);
  if (edge_it == graph.edges.end()) {
    (*memo)[pos] = -1;
    return false;
  }

  std::vector<EdgeChoice> choices;
  for (const auto& [end, syllables] : edge_it->second) {
    if (end <= pos || end > graph.interpreted_length)
      continue;
    for (const auto& [syllable_id, props] : syllables) {
      // Completion represents an unfinished syllable. Explicit dictionary
      // entries require a complete code, while configured fuzzy spellings and
      // abbreviations remain acceptable input forms.
      if (props.type > kAbbreviation)
        continue;
      choices.push_back({end, syllable_id, props.type, props.credibility});
    }
  }
  std::sort(choices.begin(), choices.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.type != rhs.type)
      return lhs.type < rhs.type;
    if (lhs.end != rhs.end)
      return lhs.end > rhs.end;
    return lhs.credibility > rhs.credibility;
  });

  for (const auto& choice : choices) {
    path->push_back(choice.syllable_id);
    if (FindSyllablePath(graph, choice.end, path, memo))
      return true;
    path->pop_back();
  }

  (*memo)[pos] = -1;
  return false;
}

bool NormalizeScriptCode(Schema* schema,
                         const std::string& name_space,
                         const std::string& input,
                         std::string* normalized,
                         the<Dictionary>* dictionary_out) {
  if (!schema || !normalized || !dictionary_out)
    return false;

  Ticket ticket(schema, name_space);
  auto* dictionary_component = Dictionary::Require("dictionary");
  if (!dictionary_component)
    return false;

  the<Dictionary> dictionary(dictionary_component->Create(ticket));
  if (!dictionary || !dictionary->Load() || !dictionary->prism())
    return false;

  TranslatorOptions options(ticket);
  Syllabifier syllabifier(options.delimiters(), false,
                          options.strict_spelling());
  SyllableGraph graph;
  const int consumed =
      syllabifier.BuildSyllableGraph(input, *dictionary->prism(), &graph);
  if (consumed < 0 || static_cast<size_t>(consumed) != input.size() ||
      graph.interpreted_length != input.size()) {
    return false;
  }

  Code path;
  std::vector<int> memo(input.size() + 1, 0);
  if (!FindSyllablePath(graph, 0, &path, &memo) || path.empty())
    return false;

  std::vector<std::string> syllables;
  if (!dictionary->Decode(path, &syllables) || syllables.size() != path.size())
    return false;

  normalized->clear();
  for (const auto& syllable : syllables) {
    if (syllable.empty())
      return false;
    normalized->append(syllable);
    normalized->push_back(' ');
  }
  *dictionary_out = std::move(dictionary);
  return true;
}

bool UpdateUserDictionary(Schema* schema,
                          const TranslatorSpec& spec,
                          const std::string& text,
                          const std::string& raw_code) {
  Ticket ticket(schema, spec.name_space);
  auto* user_dictionary_component = UserDictionary::Require("user_dictionary");
  if (!user_dictionary_component)
    return false;

  the<UserDictionary> user_dict(user_dictionary_component->Create(ticket));
  if (!user_dict || !user_dict->Load() || user_dict->readonly())
    return false;

  std::string normalized_code;
  if (spec.klass == "table_translator") {
    normalized_code = raw_code;
    boost::algorithm::trim(normalized_code);
    if (normalized_code.empty())
      return false;
    normalized_code.push_back(' ');
  } else if (spec.klass == "script_translator") {
    the<Dictionary> dictionary;
    if (!NormalizeScriptCode(schema, spec.name_space, raw_code,
                             &normalized_code, &dictionary)) {
      return false;
    }
    user_dict->Attach(dictionary->primary_table(), dictionary->prism());
  } else {
    return false;
  }

  // Ordinary learning can keep a short-lived transaction so Backspace may
  // undo a recent commit. An explicit dictionary edit must not join that
  // rollback window, so finish any pending learning transaction first.
  user_dict->CommitPendingTransaction();

  DictEntry entry;
  entry.text = text;
  entry.custom_code = normalized_code;
  return user_dict->UpdateEntry(entry, 1);
}

Bool AddUserEntry(RimeSessionId session_id,
                  const char* text,
                  const char* code) {
  if (!session_id || !text || !code)
    return False;

  std::string word(text);
  std::string input(code);
  boost::algorithm::trim(word);
  boost::algorithm::trim(input);
  if (word.empty() || input.empty() || word.size() > kMaxUserEntryTextBytes ||
      input.size() > kMaxUserEntryCodeBytes || !IsSafeUserDbField(word) ||
      !IsSafeUserDbField(input)) {
    return False;
  }

  auto session = Service::instance().GetSession(session_id);
  if (!session)
    return False;
  Schema* schema = session->schema();
  if (!schema || !schema->config())
    return False;

  auto translators = schema->config()->GetList("engine/translators");
  if (!translators)
    return False;

  for (size_t i = 0; i < translators->size(); ++i) {
    auto value = As<ConfigValue>(translators->GetAt(i));
    if (!value)
      continue;
    const TranslatorSpec spec = ParseTranslatorSpec(value->str());
    if (spec.klass != "table_translator" &&
        spec.klass != "script_translator") {
      continue;
    }
    if (UpdateUserDictionary(schema, spec, word, input)) {
      if (Context* context = session->context(); context && context->IsComposing())
        context->RefreshNonConfirmedComposition();
      return True;
    }
  }

  return False;
}

RimeCustomApi* GetUserDictionaryApi() {
  static RimeUserDictionaryApi api = {0};
  if (!api.data_size) {
    RIME_STRUCT_INIT(RimeUserDictionaryApi, api);
    api.add_user_entry = &AddUserEntry;
  }
  return reinterpret_cast<RimeCustomApi*>(&api);
}

}  // namespace

static void rime_user_dictionary_api_initialize() {}
static void rime_user_dictionary_api_finalize() {}

RIME_REGISTER_CUSTOM_MODULE(user_dictionary_api) {
  module->get_api = &GetUserDictionaryApi;
}

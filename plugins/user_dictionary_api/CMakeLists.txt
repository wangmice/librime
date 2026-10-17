set(plugin_name rime-user-dictionary-api)

add_library(rime-user-dictionary-api-objs OBJECT user_dictionary_api.cc)
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-user-dictionary-api-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_objs $<TARGET_OBJECTS:rime-user-dictionary-api-objs>)
set(plugin_deps ${rime_library} ${rime_dict_library} ${rime_gears_library})
set(plugin_modules user_dictionary_api)

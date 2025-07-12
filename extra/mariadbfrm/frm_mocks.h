#ifndef FRM_MOCKS_H
#define FRM_MOCKS_H

// This header only declares what we need for mocking
// NO type redefinitions to avoid conflicts with SQL headers

// Forward declarations only - let SQL headers define the real types
struct handlerton;
struct st_plugin_int;
typedef st_plugin_int **plugin_ref;

// Mock objects (will be defined with real types in frm_mocks.cc)
extern handlerton mock_handlerton;
extern st_plugin_int mock_plugin_int;
extern plugin_ref mock_plugin_ref;


// Plugin system initialization
void plugin_mutex_init();

#endif

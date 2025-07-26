#ifdef __EMSCRIPTEN__
EM_JS (void, listener, (void), { window.addEventListener ('beforeunload', Module.beforeunload); });
#endif

int main () { return 0; }

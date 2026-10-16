#include "ui/app.h"
#include <cstdio>
#include <signal.h>

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    canopy::ui::App app;
    std::string err;
    if (!app.init(argc, argv, &err)) {
        fprintf(stderr, "canopy: %s\n", err.c_str());
        return 1;
    }
    return app.run();
}

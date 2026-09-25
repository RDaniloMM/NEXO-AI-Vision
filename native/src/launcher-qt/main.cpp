// SPDX-License-Identifier: AGPL-3.0-only

#include "launcher_window.hpp"

#include <QApplication>
#include <QCommandLineParser>

#ifndef NEXOAI_QT_VERSION
#define NEXOAI_QT_VERSION "0.0.0.0-dev"
#endif

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName("NexoAI Vision Launcher");
    application.setApplicationVersion(NEXOAI_QT_VERSION);
    application.setOrganizationName("NexoAI Vision");

    QCommandLineParser parser;
    parser.setApplicationDescription("NexoAI Vision multicamera analytics launcher");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(application);

    LauncherWindow window;
    window.show();
    return application.exec();
}

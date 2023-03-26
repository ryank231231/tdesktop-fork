/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include <QtPlugin>
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
// Q_IMPORT_PLUGIN(QGenericEnginePlugin)
Q_IMPORT_PLUGIN(QWebpPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
// Q_IMPORT_PLUGIN(QAVIFPlugin)
// Q_IMPORT_PLUGIN(HEIFPlugin)
// Q_IMPORT_PLUGIN(QJpegXLPlugin)


#include "core/launcher.h"

int main(int argc, char *argv[]) {
	const auto launcher = Core::Launcher::Create(argc, argv);
	return launcher ? launcher->exec() : 1;
}

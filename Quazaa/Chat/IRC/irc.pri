######################################################################
# Communi: src.pri
######################################################################

DEPENDPATH += $$PWD
INCLUDEPATH += $$PWD
QMAKE_CLEAN += $$PWD/*~

FORMS += $$PWD/messageview.ui

HEADERS += $$PWD/connection.h
HEADERS += $$PWD/lineeditor.h
HEADERS += $$PWD/maintabwidget.h
HEADERS += $$PWD/messageview.h
HEADERS += $$PWD/session.h
HEADERS += $$PWD/sessiontabwidget.h
HEADERS += $$PWD/welcomepage.h

SOURCES += $$PWD/connection.cpp
SOURCES += $$PWD/lineeditor.cpp
SOURCES += $$PWD/maintabwidget.cpp
SOURCES += $$PWD/messageview.cpp
SOURCES += $$PWD/session.cpp
SOURCES += $$PWD/sessiontabwidget.cpp
SOURCES += $$PWD/welcomepage.cpp

include(3rdparty/3rdparty.pri)
include(shared/shared.pri)
include(util/util.pri)
include(wizard/wizard.pri)





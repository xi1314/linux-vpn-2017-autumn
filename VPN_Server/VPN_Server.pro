TEMPLATE = app
CONFIG += console c++11
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += src/main.cpp \
    src/vpn_server.cpp \
    src/tunnel_mgr.cpp \
    src/ip_manager.cpp

HEADERS += \
    src/ip_manager.hpp \
    src/vpn_server.hpp \
    src/client_parameters.hpp \
    src/tunnel_mgr.hpp

LIBS += -lpthread \
        -lwolfssl \

DISTFILES += \
    other.txt

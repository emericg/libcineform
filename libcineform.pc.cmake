prefix=${CMAKE_INSTALL_PREFIX}
exec_prefix=${EXEC_INSTALL_PREFIX}
libdir=${LIB_INSTALL_DIR}
includedir=${INCLUDE_INSTALL_DIR}

Name: ${PROJECT_NAME}
Description: CineForm decoding and encoding library
URL: https://github.com/egrange/libcineform
Version: ${PROJECT_VERSION}
Libs: -L${LIB_INSTALL_DIR} -lcineform ${ADDITIONAL_LIBS}
Cflags: -I${INCLUDE_INSTALL_DIR}

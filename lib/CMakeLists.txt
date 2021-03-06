include(CMakeDependentOption)

include_directories(
		${CMAKE_SOURCE_DIR}/include
		${OWFAT_INCLUDE_DIRS}
)

set(QSMTP_IO_LIB_SRCS
	libowfatconn.c
	log.c
	netio.c
	qdns.c
	ssl_timeoutio.c
	tls.c
)

set(QSMTP_IO_LIB_HDRS
	../include/log.h
	../include/netio.h
	../include/qdns.h
	../include/ssl_timeoutio.h
	../include/tls.h
)

add_library(qsmtp_io_lib ${QSMTP_IO_LIB_SRCS} ${QSMTP_IO_LIB_HDRS})
target_link_libraries(qsmtp_io_lib
		${OPENSSL_LIBRARIES}
		${OWFAT_LIBRARIES}
)

CMAKE_DEPENDENT_OPTION(REALLY_NO_LOGGING "Tell me that you intentionally disabled all logging and want to get rid of the warnings" OFF
		"NOSTDERR;NOT USESYSLOG" OFF)

if(NOSTDERR)
	if(NOT USESYSLOG AND NOT REALLY_NO_LOGGING)
		set_source_files_properties(log.c PROPERTIES COMPILE_DEFINITIONS NOSTDERR)
		message(WARNING "Neither syslog nor stderr is used for logging, you may want to enable one using the USESYSLOG or NOSTDERR options")
	elseif(USESYSLOG)
		set_source_files_properties(log.c PROPERTIES COMPILE_DEFINITIONS NOSTDERR)
	else()
		set_source_files_properties(log.c PROPERTIES COMPILE_DEFINITIONS "NOSTDERR;REALLY_NO_LOGGING")
	endif()
endif()

set(QSMTP_LIB_SRCS
	dns_helpers.c
	control.c
	base64.c
	ipme.c
	match.c
	cdb.c
	mmap.c
	fmt.c
)

set(QSMTP_LIB_HDRS
	../include/base64.h
	../include/cdb.h
	../include/control.h
	../include/fmt.h
	../include/ipme.h
	../include/match.h
	../include/mime_chars.h
	../include/mmap.h
	../include/sstring.h
	${CMAKE_BINARY_DIR}/version.h
)

add_library(qsmtp_lib ${QSMTP_LIB_SRCS} ${QSMTP_LIB_HDRS})

# qsmtp_lib is not linked against qsmtp_io_lib even if that
# would be the right thing for the binaries. This allows the
# testcases to easily link against qsmtp_lib and implementing
# the io stuff themself.

add_library(qsmtp_dane_lib
	qdns_dane.c
	../include/qdns_dane.h
)
target_link_libraries(qsmtp_dane_lib
		qsmtp_lib
		${OWFAT_LIBRARIES}
)

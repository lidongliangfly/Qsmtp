##
## Qsmtp CTest script
##
## This will run a Nightly build on Qsmtp
##

##
## What you need:
##
## All platforms:
## -cmake >= 2.8.3
## -Subversion command line client
## -all the other stuff needed to build Qsmtp like openssl, compiler, ...
##

##
## How to setup:
##
## Write to a file my_qsmtp.cmake:
##
## ######### begin file
## # the binary directory does not need to exist (but it's parent)
## # it will be deleted before use
## SET(QSMTP_BUILD_DIR "my/path/to/the/build/dir")
##
## # if you don't want to run a Nightly, but e.g. an Experimental build
## # SET(dashboard_model "Experimental")
##
## # if your "svn" executable can not be found by FindSubversion.cmake
## # SET(SVNCommand "path/to/my/svn")
##
## # if you only want to run the test, but not submit the results
## SET(NO_SUBMIT TRUE)
##
## # This _*MUST*_ be the last command in this file!
## INCLUDE(/path/to/Qsmtp/ctest_qsmtp.cmake)
## ######### end file
##
## Then run this script with
## ctest -S my_qsmtp_nightly.cmake -V
##

# Check for required variables.
FOREACH (req
                QSMTP_BUILD_DIR
        )
        IF (NOT DEFINED ${req})
                MESSAGE(FATAL_ERROR "The containing script must set ${req}")
        ENDIF ()
ENDFOREACH (req)

CMAKE_MINIMUM_REQUIRED(VERSION 2.8.3)

IF (NOT SVNCommand)
	FIND_PACKAGE(Subversion REQUIRED)
	SET(SVNCommand ${Subversion_SVN_EXECUTABLE})
ENDIF(NOT SVNCommand)
SET(UpdateCommand ${SVNCommand})

SET(CTEST_SOURCE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})
SET(CTEST_BINARY_DIRECTORY ${QSMTP_BUILD_DIR})

# Select the model (Nightly, Experimental, Continuous).
IF (NOT DEFINED dashboard_model)
        SET(dashboard_model Nightly)
ENDIF()
IF (NOT "${dashboard_model}" MATCHES "^(Nightly|Experimental|Continuous)$")
        MESSAGE(FATAL_ERROR "dashboard_model must be Nightly, Experimental, or Continuous")
ENDIF()

IF (NOT CTEST_CMAKE_GENERATOR)
	SET(CTEST_CMAKE_GENERATOR "Unix Makefiles")
ENDIF (NOT CTEST_CMAKE_GENERATOR)

# set the site name
IF (NOT CTEST_SITE)
	EXECUTE_PROCESS(COMMAND hostname --fqdn
			OUTPUT_VARIABLE _MACHINE_NAME)
	STRING(STRIP ${_MACHINE_NAME} CTEST_SITE)
ENDIF (NOT CTEST_SITE)

# set the build name
IF(NOT CTEST_BUILD_NAME)
	IF (EXISTS /etc/SuSE-release)
		FILE(STRINGS /etc/SuSE-release _SUSEVERSION)
		LIST(GET _SUSEVERSION 0 _BUILDNAMETMP)
		STRING(REGEX REPLACE "[\\(\\)]" "" CTEST_BUILD_NAME ${_BUILDNAMETMP})
	ELSE (EXISTS /etc/SuSE-release)
		MESSAGE(FATAL_ERROR "CTEST_BUILD_NAME not set.\nPlease set this to a sensible value, preferably in the form \"distribution version architecture\", something like \"openSUSE 11.3 i586\"")
	ENDIF (EXISTS /etc/SuSE-release)
ENDIF(NOT CTEST_BUILD_NAME)

FIND_PROGRAM(CTEST_MEMORYCHECK_COMMAND valgrind)
FIND_PROGRAM(CTEST_COVERAGE_COMMAND gcov)

ctest_read_custom_files(${CMAKE_CURRENT_LIST_DIR})

ctest_empty_binary_directory(${CTEST_BINARY_DIRECTORY})

ctest_start(${dashboard_model})

ctest_update()

# avoid spamming the syslog with our messages: USE_SYSLOG off
# avoid spamming the dashboard with doxygen warnings: BUILD_DOC off
# get coverage: debug build
ctest_configure(
		OPTIONS "-DUSE_SYSLOG=Off;-DBUILD_DOC=Off;-DCMAKE_BUILD_TYPE=Debug"
)
ctest_build()

# The AUTH LOGIN wrong test will take 5 seconds where they are in a sleep,
# schedule more tests in parallel so this doesn't take too long.
ctest_test(PARALLEL_LEVEL 4)

IF (CTEST_COVERAGE_COMMAND)
	ctest_coverage()
ENDIF (CTEST_COVERAGE_COMMAND)

IF (CTEST_MEMORYCHECK_COMMAND)
	ctest_memcheck()
ENDIF (CTEST_MEMORYCHECK_COMMAND)

IF (NOT NO_SUBMIT)
	ctest_submit()
ENDIF (NOT NO_SUBMIT)


CMAKE_MINIMUM_REQUIRED (VERSION 3.12)
PROJECT (iris_demo)

SET (STDLIB stdc++)

ENABLE_TESTING ()

OPTION (ENABLE_COROUTINE "Enable Iristorm Coroutine support" ON)
OPTION (ENABLE_LUA "Enable Lua Binding support" ON)
OPTION (ENABLE_ASAN "Enable Address Sanitizer" OFF)

IF (ENABLE_ASAN)
	SET (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
	SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
ENDIF (ENABLE_ASAN)

IF (MSVC)
	# Replace /EHsc with /EHa to allow longjmp (used by luaL_error) to safely
	# unwind through C++ frames on Windows x64 without triggering terminate()
	# via __CxxFrameHandler4 when processing non-C++ (SEH) unwinds.
	STRING (REPLACE "/EHsc" "/EHa" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	IF (NOT CMAKE_CXX_FLAGS MATCHES "/EHa")
		SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHa")
	ENDIF ()
ENDIF (MSVC)

IF (EMSCRIPTEN)
	SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
	SET (CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
	SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -matomics")
	SET (CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS} -matomics")
	SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -matomics")
	SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -mbulk-memory")
	SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -atomics")
	SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -bulk-memory")
	SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pthread")
	SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s USE_PTHREADS=1")
	SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s PTHREAD_POOL_SIZE=16")
	SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s SHARED_MEMORY=1")
ENDIF (EMSCRIPTEN)

MESSAGE (STATUS "Current platform: ${CMAKE_GENERATOR_PLATFORM}")
SET (COND_VLD WIN32 AND (${CMAKE_GENERATOR_PLATFORM} EQUAL "x64" OR ${CMAKE_GENERATOR_PLATFORM} EQUAL "Win32"))

IF (COND_VLD)
	OPTION (ENABLE_VLD "Enable Visual Leak Detector" OFF)
	IF (ENABLE_VLD)
		ADD_COMPILE_DEFINITIONS (USE_VLD)
		# cmake does not allow parentheses in environment variable, so $ENV{ProgramFiles(x86)} is prohibited
		FILE (TO_CMAKE_PATH "$ENV{ProgramFiles} (x86)" PROGRAM_FILES)
		INCLUDE_DIRECTORIES ("${PROGRAM_FILES}/Visual Leak Detector/include")

		IF (CMAKE_SIZEOF_VOID_P EQUAL 8)
			LINK_DIRECTORIES ("${PROGRAM_FILES}/Visual Leak Detector/lib/Win64")
		ELSE (CMAKE_SIZEOF_VOID_P EQUAL 8)
			LINK_DIRECTORIES ("${PROGRAM_FILES}/Visual Leak Detector/lib/Win32")
		ENDIF (CMAKE_SIZEOF_VOID_P EQUAL 8)
	ENDIF (ENABLE_VLD)
ENDIF (COND_VLD)

SET (USE_LUA_VERSION "Builtin" CACHE STRING "Lua version")
SET_PROPERTY (CACHE USE_LUA_VERSION PROPERTY STRINGS "Builtin" "LuaJIT" "5.1" "5.2" "5.3" "5.4" "5.5")
SET (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

IF (ENABLE_COROUTINE)
	SET (CMAKE_CXX_STANDARD 20)
ELSE (ENABLE_COROUTINE)
	IF (ENABLE_LUA)
		SET (CMAKE_CXX_STANDARD 17)
	ELSE (ENABLE_LUA)
		SET (CMAKE_CXX_STANDARD 11)
	ENDIF (ENABLE_LUA)
ENDIF (ENABLE_COROUTINE)

IF (EMSCRIPTEN)
	FIND_PROGRAM (IRIS_NODE_EXECUTABLE node REQUIRED)
ENDIF (EMSCRIPTEN)

FUNCTION (IRIS_ADD_TEST TEST_NAME TARGET_NAME)
	IF (EMSCRIPTEN)
		ADD_TEST (NAME ${TEST_NAME} COMMAND ${IRIS_NODE_EXECUTABLE} $<TARGET_FILE:${TARGET_NAME}>)
	ELSE (EMSCRIPTEN)
		ADD_TEST (NAME ${TEST_NAME} COMMAND $<TARGET_FILE:${TARGET_NAME}>)
	ENDIF (EMSCRIPTEN)
ENDFUNCTION ()

INCLUDE_DIRECTORIES ("${PROJECT_BINARY_DIR}")
STRING (REPLACE "/" "\\" BUILTIN_SOURCE_DIR "${PROJECT_SOURCE_DIR}")

FILE (GLOB_RECURSE IRIS_DISPATCHER_DEMO_SRC
	"${PROJECT_SOURCE_DIR}/src/iris_common.h"
	"${PROJECT_SOURCE_DIR}/src/iris_common.inl"
	"${PROJECT_SOURCE_DIR}/src/iris_dispatcher.h"
	"${PROJECT_SOURCE_DIR}/test/iris_dispatcher_demo.cpp"
)

ADD_EXECUTABLE (iris_dispatcher_demo ${IRIS_DISPATCHER_DEMO_SRC})
IRIS_ADD_TEST (test_dispatcher iris_dispatcher_demo)

IF (NOT MSVC)
	TARGET_LINK_LIBRARIES (iris_dispatcher_demo m ${STDLIB} pthread)
ENDIF (NOT MSVC)

FILE (GLOB_RECURSE IRIS_SYSTEM_DEMO_SRC
	"${PROJECT_SOURCE_DIR}/src/iris_common.h"
	"${PROJECT_SOURCE_DIR}/src/iris_common.inl"
	"${PROJECT_SOURCE_DIR}/src/iris_system.h"
	"${PROJECT_SOURCE_DIR}/test/iris_system_demo.cpp"
)

ADD_EXECUTABLE (iris_system_demo ${IRIS_SYSTEM_DEMO_SRC})
IRIS_ADD_TEST (test_system iris_system_demo)

IF (NOT MSVC)
	TARGET_LINK_LIBRARIES (iris_system_demo m ${STDLIB} pthread)
ENDIF (NOT MSVC)

FILE (GLOB_RECURSE IRIS_TREE_DEMO_SRC
	"${PROJECT_SOURCE_DIR}/src/iris_common.h"
	"${PROJECT_SOURCE_DIR}/src/iris_common.inl"
	"${PROJECT_SOURCE_DIR}/src/iris_tree.h"
	"${PROJECT_SOURCE_DIR}/test/iris_tree_demo.cpp"
)

ADD_EXECUTABLE (iris_tree_demo ${IRIS_TREE_DEMO_SRC})
IRIS_ADD_TEST (test_tree iris_tree_demo)

IF (NOT MSVC)
	TARGET_LINK_LIBRARIES (iris_tree_demo m ${STDLIB} pthread)
ENDIF (NOT MSVC)

IF (ENABLE_COROUTINE)
	FILE (GLOB_RECURSE IRIS_COROUTINE_DEMO_SRC
		"${PROJECT_SOURCE_DIR}/src/iris_common.h"
		"${PROJECT_SOURCE_DIR}/src/iris_common.inl"
		"${PROJECT_SOURCE_DIR}/src/iris_dispatcher.h"
		"${PROJECT_SOURCE_DIR}/src/iris_coroutine.h"
		"${PROJECT_SOURCE_DIR}/test/iris_coroutine_demo.cpp"
	)
	
	ADD_EXECUTABLE (iris_coroutine_demo ${IRIS_COROUTINE_DEMO_SRC})
	IRIS_ADD_TEST (test_coroutine iris_coroutine_demo)

	IF (NOT MSVC)
		TARGET_LINK_LIBRARIES (iris_coroutine_demo m ${STDLIB} pthread)
	ENDIF (NOT MSVC)

	FILE (GLOB_RECURSE IRIS_ENGINE_DEMO_SRC
		"${PROJECT_SOURCE_DIR}/src/iris_common.h"
		"${PROJECT_SOURCE_DIR}/src/iris_common.inl"
		"${PROJECT_SOURCE_DIR}/src/iris_dispatcher.h"
		"${PROJECT_SOURCE_DIR}/src/iris_coroutine.h"
		"${PROJECT_SOURCE_DIR}/test/iris_engine_demo.cpp"
	)
	
	ADD_EXECUTABLE (iris_engine_demo ${IRIS_ENGINE_DEMO_SRC})
	IRIS_ADD_TEST (test_engine iris_engine_demo)

	IF (NOT MSVC)
		TARGET_LINK_LIBRARIES (iris_engine_demo m ${STDLIB} pthread)
	ENDIF (NOT MSVC)

	FILE (GLOB_RECURSE IRIS_COROUTINE_STRESS_SRC
		"${PROJECT_SOURCE_DIR}/src/iris_common.h"
		"${PROJECT_SOURCE_DIR}/src/iris_common.inl"
		"${PROJECT_SOURCE_DIR}/src/iris_dispatcher.h"
		"${PROJECT_SOURCE_DIR}/src/iris_coroutine.h"
		"${PROJECT_SOURCE_DIR}/test/iris_coroutine_stress.cpp"
	)

	ADD_EXECUTABLE (iris_coroutine_stress ${IRIS_COROUTINE_STRESS_SRC})
	IRIS_ADD_TEST (test_coroutine_stress iris_coroutine_stress)

	IF (EMSCRIPTEN)
		TARGET_LINK_OPTIONS (iris_coroutine_stress PRIVATE "SHELL:-s INITIAL_MEMORY=67108864")
	ENDIF (EMSCRIPTEN)

	IF (NOT MSVC)
		TARGET_LINK_LIBRARIES (iris_coroutine_stress m ${STDLIB} pthread)
	ENDIF (NOT MSVC)
ENDIF (ENABLE_COROUTINE)

IF (ENABLE_LUA)
	FILE (GLOB_RECURSE IRIS_LUA_DEMO_SRC
		"${PROJECT_SOURCE_DIR}/src/iris_lua.h"
		"${PROJECT_SOURCE_DIR}/test/iris_lua_demo.cpp"
	)

	IF (${USE_LUA_VERSION} STREQUAL "Builtin")
		SET (ENABLE_LUA_BUILTIN ON)
	ELSE (${USE_LUA_VERSION} STREQUAL "Builtin")
		SET (ENABLE_LUA_BUILTIN OFF)
	ENDIF (${USE_LUA_VERSION} STREQUAL "Builtin")

	IF (ENABLE_LUA_BUILTIN)
		MESSAGE (STATUS "Use built-in lua version.")
		# default to lua 5.5
		FILE (GLOB_RECURSE IRIS_LUA_CORE_SRC
			"${PROJECT_SOURCE_DIR}/src/lua/src/*.h"
			"${PROJECT_SOURCE_DIR}/src/lua/src/*.hpp"
			"${PROJECT_SOURCE_DIR}/src/lua/src/*.c"
		)

		LIST (REMOVE_ITEM IRIS_LUA_CORE_SRC "${PROJECT_SOURCE_DIR}/src/lua/src/lua.c")
		LIST (REMOVE_ITEM IRIS_LUA_CORE_SRC "${PROJECT_SOURCE_DIR}/src/lua/src/luac.c")
		SET (LUALIB)
	ELSE (ENABLE_LUA_BUILTIN)
		IF (${USE_LUA_VERSION} STREQUAL "LuaJIT")
			FIND_PACKAGE (LuaJIT)
		ELSE (${USE_LUA_VERSION} STREQUAL "LuaJIT")
			FIND_PACKAGE (Lua ${USE_LUA_VERSION} REQUIRED)
		ENDIF (${USE_LUA_VERSION} STREQUAL "LuaJIT")

		IF (LUA_FOUND OR LUAJIT_FOUND)
			MESSAGE (STATUS "Find lua version " ${USE_LUA_VERSION})
			FILE (GLOB_RECURSE IRIS_LUA_CORE_SRC
				"${LUA_INCLUDE_DIR}/*.h"
				"${LUA_INCLUDE_DIR}/*.hpp"
			)
	
			SET (LUALIB ${LUA_LIBRARIES})
		ELSE (LUA_FOUND OR LUAJIT_FOUND)
			MESSAGE (FATAL_ERROR "Unable to find lua with version " ${USE_LUA_VERSION})
		ENDIF (LUA_FOUND OR LUAJIT_FOUND)
	ENDIF (ENABLE_LUA_BUILTIN)

	ADD_EXECUTABLE (iris_lua_demo ${IRIS_LUA_DEMO_SRC} ${IRIS_LUA_CORE_SRC})

	IF (ENABLE_COROUTINE)
		TARGET_COMPILE_DEFINITIONS (iris_lua_demo PRIVATE USE_LUA_COROUTINE)
	ENDIF (ENABLE_COROUTINE)

	IF (NOT ENABLE_LUA_BUILTIN)
		TARGET_INCLUDE_DIRECTORIES (iris_lua_demo PUBLIC ${LUA_INCLUDE_DIR})
		TARGET_COMPILE_DEFINITIONS (iris_lua_demo PUBLIC USE_LUA_LIBRARY)
	ENDIF (NOT ENABLE_LUA_BUILTIN)

	TARGET_COMPILE_DEFINITIONS (iris_lua_demo PRIVATE LUAI_ASSERT)
	IRIS_ADD_TEST (test_lua iris_lua_demo)

	IF (MSVC)
		TARGET_LINK_LIBRARIES (iris_lua_demo ${LUALIB})
	ELSE (MSVC)
		TARGET_LINK_LIBRARIES (iris_lua_demo m ${STDLIB} ${LUALIB})
	ENDIF (MSVC)

	FILE (GLOB_RECURSE IRIS_LUA_PROXY_DEMO_SRC
		"${PROJECT_SOURCE_DIR}/src/iris_lua.h"
		"${PROJECT_SOURCE_DIR}/test/iris_lua_proxy_demo.cpp"
	)

	ADD_EXECUTABLE (iris_lua_proxy_demo ${IRIS_LUA_PROXY_DEMO_SRC} ${IRIS_LUA_CORE_SRC})

	IF (NOT ENABLE_LUA_BUILTIN)
		TARGET_INCLUDE_DIRECTORIES (iris_lua_proxy_demo PUBLIC ${LUA_INCLUDE_DIR})
		TARGET_COMPILE_DEFINITIONS (iris_lua_proxy_demo PUBLIC USE_LUA_LIBRARY)
	ENDIF (NOT ENABLE_LUA_BUILTIN)

	TARGET_COMPILE_DEFINITIONS (iris_lua_proxy_demo PRIVATE LUAI_ASSERT)
	IRIS_ADD_TEST (test_lua_proxy iris_lua_proxy_demo)

	IF (MSVC)
		TARGET_LINK_LIBRARIES (iris_lua_proxy_demo ${LUALIB})
	ELSE (MSVC)
		TARGET_LINK_LIBRARIES (iris_lua_proxy_demo m ${STDLIB} ${LUALIB})
	ENDIF (MSVC)
ENDIF (ENABLE_LUA)


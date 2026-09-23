if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
	set(__nvapi_package_path ${CMAKE_CURRENT_LIST_DIR}/links/nvapi)
	if (NOT DEFINED __nvapi_install)
		set(__nvapi_install ${CMAKE_CURRENT_LIST_DIR}/links/nvapi)
	endif()

	add_library(nvapi IMPORTED STATIC GLOBAL)
	if (WIN32)
		if(CMAKE_SIZEOF_VOID_P EQUAL 8)
			set_target_properties(nvapi PROPERTIES
								IMPORTED_LOCATION ${__nvapi_install}/amd64/nvapi64.lib)
		else()
			set_target_properties(nvapi PROPERTIES
								IMPORTED_LOCATION ${__nvapi_install}/x86/nvapi.lib)
		endif()
	else ()
		message(FATAL_ERROR "Need nvapi binary path for current platform --- please fix me!")
	endif()

	target_include_directories(nvapi INTERFACE ${__nvapi_install})
else()
	set(__nvapi_package_path "${DEPENDENCIES_ROOT}/nvapi")
	if (NOT DEFINED __nvapi_install)
		set(__nvapi_install  "${DEPENDENCIES_ROOT}/nvapi")
	endif()

	add_library(nvapi IMPORTED STATIC GLOBAL)
	if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR CMAKE_GENERATOR_PLATFORM STREQUAL "arm64ec")
		set_target_properties(nvapi PROPERTIES
								IMPORTED_LOCATION ${__nvapi_install}/amd64/nvapi64.lib)
	elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "arm64")
		set_target_properties(nvapi PROPERTIES
								IMPORTED_LOCATION ${__nvapi_install}/aarch64/nvapia64.lib)
	else ()
		message(FATAL_ERROR "Need nvapi binary path for current platform ${CMAKE_GENERATOR_PLATFORM} --- please fix me!")
	endif()

	target_include_directories(nvapi INTERFACE ${__nvapi_install}/include)
endif()

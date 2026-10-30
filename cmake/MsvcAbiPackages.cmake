# vcpkg ports are built with MSVC, and some export cl-style usage requirements unconditionally (spdlog: /Zc:__cplusplus,
# fmt: /Zc:preprocessor). clang++ (GNU driver, MSVC ABI) reads those as file names; its defaults already behave as the
# options ask, so they are dropped for it.
#
# basicrenderer_find_msvc_abi_packages() finds such packages once, in the calling (top-level) directory, and strips the
# options from their imported targets. Later find_package calls in subdirectories see the targets already defined and
# reuse them. Call it before any add_subdirectory that finds these packages.

function(basicrenderer_strip_cl_interface_options)
	if(MSVC)
		return()
	endif()
	foreach(_target IN LISTS ARGN)
		if(NOT TARGET ${_target})
			continue()
		endif()
		get_target_property(_aliased ${_target} ALIASED_TARGET)
		if(_aliased)
			set(_target ${_aliased})
		endif()
		get_target_property(_options ${_target} INTERFACE_COMPILE_OPTIONS)
		if(NOT _options)
			continue()
		endif()
		set(_kept)
		foreach(_option IN LISTS _options)
			if(NOT _option MATCHES "^/")
				list(APPEND _kept "${_option}")
			endif()
		endforeach()
		set_property(TARGET ${_target} PROPERTY INTERFACE_COMPILE_OPTIONS "${_kept}")
	endforeach()
endfunction()

macro(basicrenderer_find_msvc_abi_packages)
	if(NOT MSVC AND (CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
		find_package(fmt CONFIG QUIET)
		find_package(spdlog CONFIG QUIET)
		basicrenderer_strip_cl_interface_options(fmt::fmt fmt::fmt-header-only spdlog::spdlog spdlog::spdlog_header_only)
	endif()
endmacro()

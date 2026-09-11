include_guard(GLOBAL)

# Check the link interfaces of the persistent and evaluation layers after all
# targets have been declared. This is intentionally a small graph walk rather
# than a source-text policy: CMake's actual target/link relationships are the
# architecture contract.
function(nemo_check_dependency_direction)
    foreach(_nemo_root IN ITEMS nemo_core nemo_eval)
        if(NOT TARGET "${_nemo_root}")
            continue()
        endif()

        set(_nemo_queue "${_nemo_root}")
        set(_nemo_paths "${_nemo_root}")
        set(_nemo_visited)

        while(_nemo_queue)
            list(POP_FRONT _nemo_queue _nemo_current)
            list(POP_FRONT _nemo_paths _nemo_current_path)
            if(_nemo_current IN_LIST _nemo_visited)
                continue()
            endif()
            list(APPEND _nemo_visited "${_nemo_current}")

            foreach(_nemo_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
                get_target_property(_nemo_links "${_nemo_current}" "${_nemo_property}")
                if(NOT _nemo_links OR _nemo_links MATCHES "-NOTFOUND$")
                    continue()
                endif()

                foreach(_nemo_dependency IN LISTS _nemo_links)
                    if(_nemo_dependency MATCHES "^(debug|optimized|general)$")
                        continue()
                    endif()

                    # PRIVATE links can be represented as LINK_ONLY generator
                    # expressions in an interface. Unwrap the simple form so
                    # the relationship is still checked; unsupported dynamic
                    # expressions are not guessed at configure time.
                    string(REGEX REPLACE "^\\$<LINK_ONLY:([^>]+)>$" "\\1"
                           _nemo_dependency "${_nemo_dependency}")
                    string(REGEX REPLACE "^\\$<BUILD_INTERFACE:([^>]+)>$" "\\1"
                           _nemo_dependency "${_nemo_dependency}")
                    if(_nemo_dependency MATCHES "^\\$<")
                        continue()
                    endif()

                    set(_nemo_forbidden FALSE)
                    if(_nemo_root STREQUAL "nemo_core")
                        if(_nemo_dependency MATCHES
                           "^(nemo_core|nemo::core|nemo_eval|nemo::eval|nemo_gpu|nemo::gpu|nemo_media|nemo::media|nemo_workspace|nemo::workspace|nemo-ui|nemo::gpu_viewerinterop|nemo_openfx|nemo::openfx|Vulkan::.*|GPUOpen::.*|glslang::.*|SPIRV::.*|OpenFX::.*|Qt[0-9]+::.*|PkgConfig::LIBAV)$")
                            set(_nemo_forbidden TRUE)
                        endif()
                    elseif(_nemo_root STREQUAL "nemo_eval")
                        if(_nemo_dependency MATCHES
                           "^(nemo_workspace|nemo::workspace|nemo-ui|Qt[0-9]+::.*)$")
                            set(_nemo_forbidden TRUE)
                        endif()
                    endif()

                    if(_nemo_forbidden)
                        message(FATAL_ERROR
                            "Nemo dependency direction violation: '${_nemo_root}' "
                            "depends on forbidden '${_nemo_dependency}' via "
                            "${_nemo_current_path} -> ${_nemo_dependency} "
                            "(${_nemo_property}). Persistent core/evaluation "
                            "must remain independent of UI/runtime dependencies.")
                    endif()

                    if(TARGET "${_nemo_dependency}")
                        list(APPEND _nemo_queue "${_nemo_dependency}")
                        list(APPEND _nemo_paths
                             "${_nemo_current_path} -> ${_nemo_dependency}")
                    endif()
                endforeach()
            endforeach()
        endwhile()
    endforeach()
endfunction()

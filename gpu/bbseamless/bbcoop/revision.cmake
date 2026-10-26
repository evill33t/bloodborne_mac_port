# bbport co-op: writes OUT (bbcoop_revision.h) with the source tree's revision, rewritten only
# when it changed. Run at every build: the log names the build ("Run: Revision <hash>"), and the
# games tell each other theirs (PeerHello). "-dirty" marks changes not in a described commit.
set(revision "")
set(dirty FALSE)
# jj from where it is usually installed too: the MSYS2 build shell starts from its own PATH, and a
# jj workspace without .git (the release checkout, C:\bb\release-src) has nothing else to ask (the
# first v1.1.3 build named itself 0000000).
set(jj_places "")
foreach (base "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Links" "$ENV{USERPROFILE}/.cargo/bin"
              "$ENV{USERPROFILE}/scoop/shims" "$ENV{ProgramFiles}/jj")
    file(TO_CMAKE_PATH "${base}" base)
    list(APPEND jj_places "${base}")
endforeach()
file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages" winget)
file(GLOB winget_jj LIST_DIRECTORIES true "${winget}/jj-vcs.jj_*")
find_program(jj NAMES jj HINTS ${winget_jj} PATHS ${jj_places})
if (NOT jj)
    set(jj jj)
endif()
# A jj workspace first: in one colocated with git, git's HEAD is the working copy's parent, which
# is the wrong commit when the working copy is the change being built (the npcfix15 kit built
# c01b49d3 from its working copy and logged its parent 7f3343f0). The log snapshots the files, so
# edits made since the last jj command count.
execute_process(COMMAND ${jj} log --no-graph --limit 1 -r @
                        -T "commit_id ++ \" \" ++ empty ++ \" \" ++ description.first_line()"
                WORKING_DIRECTORY "${SOURCE}" OUTPUT_VARIABLE working_copy
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE jj_failed)
if (jj_failed)
    # A stale or locked workspace: its last snapshot.
    execute_process(COMMAND ${jj} log --ignore-working-copy --no-graph --limit 1 -r @
                            -T "commit_id ++ \" \" ++ empty ++ \" \" ++ description.first_line()"
                    WORKING_DIRECTORY "${SOURCE}" OUTPUT_VARIABLE working_copy
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE jj_failed)
endif()
if (NOT jj_failed AND working_copy MATCHES "^([0-9a-f]+) (true|false) ?(.*)$")
    set(commit "${CMAKE_MATCH_1}")
    set(empty "${CMAKE_MATCH_2}")
    set(described "${CMAKE_MATCH_3}")
    if (empty STREQUAL "true")
        # Nothing changed on top: the build is the parent's.
        execute_process(COMMAND ${jj} log --ignore-working-copy --no-graph --limit 1 -r @- -T commit_id
                        WORKING_DIRECTORY "${SOURCE}" OUTPUT_VARIABLE revision
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    else()
        # The working copy is a commit of its own; undescribed it is work in progress.
        set(revision "${commit}")
        if (described STREQUAL "")
            set(dirty TRUE)
        endif()
    endif()
else()
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${SOURCE}"
                    OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${SOURCE}"
                    OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (NOT changes STREQUAL "")
        set(dirty TRUE)
    endif()
endif()
if (NOT revision MATCHES "^[0-9a-f]+$")
    set(revision "0000000")
    set(dirty FALSE)
endif()
if (dirty)
    string(APPEND revision "-dirty")
endif()
set(content "#pragma once\n#define BB_SCM_REV \"${revision}\"\n")
if (EXISTS "${OUT}")
    file(READ "${OUT}" previous)
endif()
if (NOT previous STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()

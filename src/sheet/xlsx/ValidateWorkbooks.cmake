# ctest office_workbook_validate: each folder's workbooks written back from the model
# (fwtool roundtrip --model --reserialize-all --out), then the Open XML SDK validator
# (tools/validate) must find no error the original doesn't have.
#   cmake -DFWTOOL=... -DSOURCE=... -DBINARY=... -DDIRS="a;b" -P ValidateWorkbooks.cmake
execute_process(COMMAND dotnet build "${SOURCE}/tools/validate/Validate.csproj" -c Release -o "${BINARY}/validate"
                --nologo -v q RESULT_VARIABLE result)
if(result)
  message(FATAL_ERROR "building tools/validate failed")
endif()
foreach(dir ${DIRS})
  get_filename_component(name "${dir}" NAME)
  file(GLOB_RECURSE books "${dir}/*.xlsx" "${dir}/*.xlsm" "${dir}/*.xltx" "${dir}/*.xltm")
  if(NOT books)
    continue()
  endif()
  set(out "${BINARY}/validate-saved/${name}")
  file(REMOVE_RECURSE "${out}")
  file(MAKE_DIRECTORY "${out}")
  execute_process(COMMAND "${FWTOOL}" roundtrip --model --reserialize-all --out "${out}" --workbooks "${dir}"
                  RESULT_VARIABLE result OUTPUT_QUIET)
  if(result)
    message(FATAL_ERROR "fwtool roundtrip failed on ${dir}")
  endif()
  execute_process(COMMAND "${BINARY}/validate/fwvalidate.exe" --saved "${out}" "${dir}" RESULT_VARIABLE result
                  OUTPUT_VARIABLE output)
  message("${name}: ${output}")
  if(result)
    message(FATAL_ERROR "new validator errors in ${dir}")
  endif()
endforeach()

# Links the allocation guard with WHOLE_ARCHIVE for the same reason the tests
# do: a global operator new replacement resolves no undefined symbol, so an
# ordinary static link would drop the object and the report would claim zero
# violations because it was never watching.
add_executable(rt_cycle_report rt_cycle_report.cpp)
target_link_libraries(rt_cycle_report PRIVATE
  safeedge::rt
  safeedge::warnings
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,safeedge::rt_alloc_guard>")

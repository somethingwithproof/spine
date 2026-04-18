file(REMOVE_RECURSE
  "libspine_platform_test_support.a"
  "libspine_platform_test_support.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang C)
  include(CMakeFiles/spine_platform_test_support.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()

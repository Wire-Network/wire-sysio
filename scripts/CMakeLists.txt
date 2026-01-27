
configure_file(abi_is_json.py abi_is_json.py COPYONLY)
configure_file(postinst . @ONLY)
configure_file(prerm . @ONLY)
configure_file(install_testharness_symlinks.cmake .)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/completions/clio.bash
  DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/bash-completion/completions
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/completions/_clio
  DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/zsh/site-functions
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/completions/clio.fish
  DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/fish/vendor_completions.d
)

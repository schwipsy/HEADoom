add_pk3(HEADoom.pk3 ${CMAKE_CURRENT_SOURCE_DIR}/static)

generate_language_files(HEADoom_pk3 static/language.csv ENGINE)

# insert appstream data into HEADoom.pk3, for release notes
add_custom_target(
	copy_metadata
	COMMAND ${CMAKE_COMMAND} -E copy
		"${CMAKE_CURRENT_SOURCE_DIR}/../src/posix/freedesktop/org.zdoom.UZDoom.metainfo.xml"
		"${CMAKE_CURRENT_SOURCE_DIR}/static/meta.xml"
	COMMAND ${CMAKE_COMMAND} -E copy
		"${CMAKE_CURRENT_SOURCE_DIR}/../CONTRIBUTORS"
		"${CMAKE_CURRENT_SOURCE_DIR}/static/contributors.txt"
)
add_dependencies(HEADoom_pk3 copy_metadata)
add_custom_command(
	TARGET HEADoom_pk3
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E remove
		"${CMAKE_CURRENT_SOURCE_DIR}/static/meta.xml"
	COMMAND ${CMAKE_COMMAND} -E remove
		"${CMAKE_CURRENT_SOURCE_DIR}/static/contributors.txt"
)

project(DrawMatrix)
set(SOURCE main.cpp my_glut.h)
find_package(OpenGL)
find_package(GLUT)

if(OPENGL_FOUND)
  if(GLUT_FOUND)
    message("linking DrawMatrix with GLUT and OpenGL")
    include_directories(${OPENGL_INCLUDE_DIR})
    include_directories(${GLUT_INCLUDE_DIR})
    add_executable(DrawMatrix ${SOURCE})
    target_link_libraries(DrawMatrix inmost ${OPENGL_LIBRARIES} ${GLUT_LIBRARIES})
    if(USE_MPI)
      target_link_libraries(DrawMatrix ${MPI_LIBRARIES}) 
      if(MPI_LINK_FLAGS)
        set_target_properties(DrawMatrix PROPERTIES LINK_FLAGS "${MPI_LINK_FLAGS}")
      endif() 
    endif(USE_MPI)
    


    install(TARGETS DrawMatrix EXPORT inmost-targets RUNTIME DESTINATION bin)
  else(GLUT_FOUND)
    message("GLUT not found, not building DrawMatrix")
  endif(GLUT_FOUND)
else(OPENGL_FOUND)
  message("OpenGL not found, not building DrawMatrix")
endif(OPENGL_FOUND)

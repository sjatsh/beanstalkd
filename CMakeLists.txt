cmake_minimum_required(VERSION 3.17)
project(beanstalkd C)

set(BUILD_DIR .)


add_custom_target(beanstalkd ALL COMMAND make WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_custom_command(TARGET beanstalkd POST_BUILD
        COMMAND echo copy ${PROJECT_NAME} to ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND cp ${CMAKE_CURRENT_SOURCE_DIR}/beanstalkd ${CMAKE_CURRENT_BINARY_DIR}
        )
add_executable(beanstalkd-exec
        conn.c
        freebsd.c
        linux.c
        sunos.c
        darwin.c
        dat.h
        file.c
        heap.c
        job.c
        main.c
        ms.c
        net.c
        primes.c
        prot.c
        serv.c
        time.c
        tube.c
        util.c
        walg.c)


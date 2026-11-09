set(TRANSPORT_SRCS
    src/Protocol.cc
    src/Codec.cc
    src/TransferSession.cc
    src/FileTransferServer.cc
    src/FileTransferClient.cc
    p2p/src/Sha1.cc
    p2p/src/NodeId.cc
    p2p/src/RoutingTable.cc
    p2p/src/KademliaNode.cc
    p2p/src/TorrentMeta.cc
    p2p/src/PieceStore.cc
    p2p/src/PeerWire.cc
    p2p/src/PeerSession.cc
    p2p/src/TorrentSession.cc
)

add_library(transport STATIC ${TRANSPORT_SRCS})

target_include_directories(transport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/p2p/include
        ${CMAKE_SOURCE_DIR}/include
)

find_package(OpenSSL)
if(OpenSSL_FOUND)
    target_link_libraries(transport PUBLIC muduo_core ${LIBS} OpenSSL::Crypto)
    target_compile_definitions(transport PUBLIC P2P_HAVE_OPENSSL=1)
else()
    target_link_libraries(transport PUBLIC muduo_core ${LIBS})
endif()
target_compile_options(transport PRIVATE -Wall)

set_target_properties(transport PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib
)

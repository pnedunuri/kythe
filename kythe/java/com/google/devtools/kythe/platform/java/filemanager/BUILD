package(default_visibility = ["//kythe:default_visibility"])

java_library(
    name = "filemanager",
    srcs = [
        "CompilationUnitBasedJavaFileManager.java",
        "CompilationUnitBasedJavaFileStore.java",
        "CompilationUnitFileTree.java",
        "CustomFileObject.java",
        "CustomJavaFileObject.java",
        "JavaFileStore.java",
        "JavaFileStoreBasedFileManager.java",
    ],
    deps = [
        "//kythe/java/com/google/devtools/kythe/extractors/java",
        "//kythe/java/com/google/devtools/kythe/platform/shared:file_data",
        "//kythe/proto:analysis_proto_java",
        "//kythe/proto:java_proto_java",
        "//third_party/proto:any_proto_java",
        "//third_party/proto:protobuf_java",
        "//third_party/javac",
    ],
)

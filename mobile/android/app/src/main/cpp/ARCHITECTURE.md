# mobile/android/app/src/main/cpp Architecture

`mobile/android/app/src/main/cpp/` owns Android JNI/native helpers.

Use this directory only when Android needs shared native behavior or a narrow
native adapter. Kotlin should remain the owner of Android UI and platform event
wiring.

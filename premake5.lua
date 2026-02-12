workspace "ToyStory2Fix"
   configurations { "Release", "Debug" }
   platforms { "Win32" }
   architecture "x32"
   location "build"
   objdir ("build/obj")
   buildlog ("build/log/%{prj.name}.log")
   buildoptions {"-std:c++17"}

   function applycommon()
      language "C++"
      characterset ("MBCS")
      flags { "StaticRuntime" }
      includedirs { "includes" }
      includedirs { "external/hooking" }
      includedirs { "external/injector/include" }
      includedirs { "external/inireader" }
      -- dxguid provides DirectX IID/CLSID definitions without importing ddraw.dll.
      links { "winmm", "dxguid" }
   end

   function applyversiondefines()
      defines { "rsc_CompanyName=\"RibShark\"" }
      defines { "rsc_LegalCopyright=\"MIT License\""} 
      defines { "rsc_FileVersion=\"1.0.0.0\"", "rsc_ProductVersion=\"1.0.0.0\"" }
      defines { "rsc_InternalName=\"%{prj.name}\"", "rsc_ProductName=\"%{prj.name}\"", "rsc_OriginalFilename=\"%{prj.name}.asi\"" }
      defines { "rsc_FileDescription=\"\"" }
      defines { "rsc_UpdateUrl=\"https://github.com/RibShark/ToyStory2Fix\"" }
   end

   pbcommands = { 
      "setlocal EnableDelayedExpansion",
      --"set \"path=" .. (gamepath) .. "\"",
      "set file=$(TargetPath)",
      "FOR %%i IN (\"%file%\") DO (",
      "set filename=%%~ni",
      "set fileextension=%%~xi",
      "set target=!path!!filename!!fileextension!",
      "if exist \"!target!\" copy /y \"!file!\" \"!target!\"",
      ")" }

   function setpaths (gamepath, exepath, scriptspath)
      scriptspath = scriptspath or "scripts/"
      if (gamepath) then
         cmdcopy = { "set \"path=" .. gamepath .. scriptspath .. "\"" }
         table.insert(cmdcopy, pbcommands)
         postbuildcommands (cmdcopy)
         debugdir (gamepath)
         if (exepath) then
            debugcommand (gamepath .. exepath)
            dir, file = exepath:match'(.*/)(.*)'
            debugdir (gamepath .. (dir or ""))
         end
      end
      targetdir ("data/" .. scriptspath)
   end
   
   filter "configurations:Debug"
      defines "DEBUG"
      symbols "On"

   filter "configurations:Release"
      defines "NDEBUG"
      optimize "On"

   filter {}

project "ToyStory2Fix"
   kind "SharedLib"
   targetdir "data/scripts"
   targetextension ".asi"
   applycommon()
   applyversiondefines()
   files { "source/*.cpp" }
   files { "resources/*.rc" }
   files { "external/hooking/Hooking.Patterns.h", "external/hooking/Hooking.Patterns.cpp" }
   files { "includes/stdafx.h", "includes/stdafx.cpp" }
   setpaths("D:/Games/Toy Story 2/", "toy2.exe")

project "ToyStory2DepthWrapper"
   kind "SharedLib"
   targetname "ddraw"
   targetdir "data"
   targetextension ".dll"
   applycommon()
   files { "wrapper_source/*.cpp", "wrapper_source/*.def" }
   files { "external/hooking/Hooking.Patterns.h", "external/hooking/Hooking.Patterns.cpp" }
   files { "includes/stdafx.h" }
   files { "source/config.cpp", "source/frame_timer.cpp", "source/frame_timer_install.cpp", "source/logging.cpp", "source/pattern_utils.cpp", "source/runtime.cpp", "source/zero_speed_safety.cpp" }

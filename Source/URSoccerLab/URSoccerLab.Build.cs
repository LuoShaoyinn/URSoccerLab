// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

	public class URSoccerLab : ModuleRules
	{
		public URSoccerLab(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

			PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "URLab" });

			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Json",
				"JsonUtilities",
                "XmlParser",
                "glTFRuntime",
				"Sockets",
				"ImageWrapper",
				"ImageCore",
				"DisplayCluster",
				"DisplayClusterProjection",
				"RenderCore",
				"RHI",
				"Slate",
				"SlateCore"
			});

			// FFmpeg provides Vulkan AV1 encoding; isolate its C headers from the
            // engine toolchain's standard-library include paths.
            string ffmpegRoot = System.Environment.GetEnvironmentVariable("URS_FFMPEG_ROOT") ?? "/usr";
            string ffmpegInclude = System.IO.Path.GetFullPath(System.IO.Path.Combine(ModuleDirectory, "../../Intermediate/ThirdParty/FFmpeg/include"));
            foreach (string component in new[] { "libavcodec", "libavutil", "libswscale" })
            {
                string source = System.IO.Path.Combine(ffmpegRoot, "include", component);
                string destination = System.IO.Path.Combine(ffmpegInclude, component);
                if (!System.IO.Directory.Exists(source))
                    throw new BuildException("Install FFmpeg development headers or set URS_FFMPEG_ROOT: " + source);
                System.IO.Directory.CreateDirectory(destination);
                foreach (string header in System.IO.Directory.GetFiles(source, "*.h"))
                    System.IO.File.Copy(header, System.IO.Path.Combine(destination, System.IO.Path.GetFileName(header)), true);
                string library = System.IO.Path.Combine(ffmpegRoot, "lib", component + ".so");
                if (!System.IO.File.Exists(library)) library = System.IO.Path.Combine(ffmpegRoot, "lib/x86_64-linux-gnu", component + ".so");
                PublicAdditionalLibraries.Add(library);
            }
            PublicSystemIncludePaths.Add(ffmpegInclude);

            // yyjson — vendored single-file JSON library (much faster than UE Json)
			string yyjsonDir = System.IO.Path.Combine(ModuleDirectory, "ThirdParty", "yyjson");
			PublicIncludePaths.Add(System.IO.Path.Combine(yyjsonDir, "include"));
		}
	}

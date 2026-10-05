#include "Scene/URSExternalRobot.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MuJoCo/Components/Actuators/MjActuator.h"
#include "MuJoCo/Components/Bodies/MjBody.h"
#include "MuJoCo/Components/Joints/MjJoint.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "MuJoCo/Utils/MjUtils.h"
#include "Serialization/JsonSerializer.h"
#include "XmlFile.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"

namespace
{
struct FNode
{
	FString Tag;
	TMap<FString, FString> Attributes;
	TArray<TSharedPtr<FNode>> Children;
	FString Get(const TCHAR *Key) const
	{
		const auto *V = Attributes.Find(Key);
		return V ? *V : FString();
	}
};
FString Absolute(const FString &Base, const FString &Path)
{
	return FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::Combine(Base, Path) : Path);
}
TSharedPtr<FNode> Expand(const FXmlNode *Xml, const FString &Directory, TSet<FString> &Stack, FString &Error)
{
	auto Node = MakeShared<FNode>();
	Node->Tag = Xml->GetTag();
	for (const auto &A : Xml->GetAttributes())
		Node->Attributes.Add(A.GetTag(), A.GetValue());
	if (Node->Get(TEXT("name")).StartsWith(TEXT("__urs_visual_")))
	{
		Error = TEXT("MJCF names beginning __urs_visual_ are reserved for the robot loader");
		return nullptr;
	}
	for (const FXmlNode *Child : Xml->GetChildrenNodes())
	{
		if (Child->GetTag() == TEXT("include"))
		{
			FString File = Absolute(Directory, Child->GetAttribute(TEXT("file")));
			if (Stack.Contains(File))
			{
				Error = TEXT("recursive MJCF include: ") + File;
				return nullptr;
			}
			FXmlFile Included(File);
			if (!Included.IsValid())
			{
				Error = TEXT("cannot read MJCF include: ") + File;
				return nullptr;
			}
			Stack.Add(File);
			auto Expanded = Expand(Included.GetRootNode(), FPaths::GetPath(File), Stack, Error);
			Stack.Remove(File);
			if (!Expanded)
				return nullptr;
			Node->Children.Append(Expanded->Children);
		}
		else
		{
			auto Expanded = Expand(Child, Directory, Stack, Error);
			if (!Expanded)
				return nullptr;
			Node->Children.Add(Expanded);
		}
	}
	return Node;
}
FString Escape(FString V)
{
	V.ReplaceInline(TEXT("&"), TEXT("&amp;"));
	V.ReplaceInline(TEXT("\""), TEXT("&quot;"));
	V.ReplaceInline(TEXT("<"), TEXT("&lt;"));
	V.ReplaceInline(TEXT(">"), TEXT("&gt;"));
	return V;
}
FString Serialize(const FNode &Node)
{
	FString S = TEXT("<") + Node.Tag;
	for (const auto &A : Node.Attributes)
		S += TEXT(" ") + A.Key + TEXT("=\"") + Escape(A.Value) + TEXT("\"");
	if (Node.Children.IsEmpty())
		return S + TEXT("/>");
	S += TEXT(">");
	for (const auto &Child : Node.Children)
		S += Serialize(*Child);
	return S + TEXT("</") + Node.Tag + TEXT(">");
}
void Defaults(const FNode &Node, const TMap<FString, FString> &Parent,
              TMap<FString, TMap<FString, FString>> &Out)
{
	if (Node.Tag != TEXT("default"))
	{
		for (auto &C : Node.Children)
			Defaults(*C, Parent, Out);
		return;
	}
	auto Effective = Parent;
	for (auto &C : Node.Children)
		if (C->Tag == TEXT("geom"))
		{
			const TArray<FString> Keys = {TEXT("quat"), TEXT("axisangle"), TEXT("euler"), TEXT("xyaxes"),
			                              TEXT("zaxis")};
			if (Keys.ContainsByPredicate([&](const FString &Key) { return C->Attributes.Contains(Key); }))
				for (const auto &Key : Keys)
					Effective.Remove(Key);
			for (auto &A : C->Attributes)
				Effective.Add(A.Key, A.Value);
		}
	const FString Name = Node.Get(TEXT("class"));
	Out.Add(Name, Effective);
	for (auto &C : Node.Children)
		if (C->Tag == TEXT("default"))
			Defaults(*C, Effective, Out);
}
bool Numbers(const FString &S, int32 Count, TArray<double> &Values)
{
	TArray<FString> Parts;
	S.ParseIntoArrayWS(Parts);
	if (Parts.Num() != Count)
		return false;
	for (const auto &P : Parts)
	{
		double V;
		if (!LexTryParseString(V, *P) || !FMath::IsFinite(V))
			return false;
		Values.Add(V);
	}
	return true;
}
struct FMesh
{
	FString File;
	FVector Scale = FVector::OneVector;
};
bool Transform(FNode &Node, const FString &ChildClass, const TMap<FString, FMesh> &Meshes,
               const TMap<FString, TMap<FString, FString>> &Defs, URSoccerLab::FExternalRobotPackage &Package,
               FString &Error)
{
	if (Node.Tag == TEXT("default"))
		return true;
	FString InheritedClass = ChildClass;
	if ((Node.Tag == TEXT("body") || Node.Tag == TEXT("frame")) &&
	    Node.Attributes.Contains(TEXT("childclass")))
		InheritedClass = Node.Get(TEXT("childclass"));
	if (Node.Tag == TEXT("body"))
	{
		bool Joint = false, Inertial = false;
		for (auto &C : Node.Children)
		{
			Joint |= C->Tag == TEXT("joint") || C->Tag == TEXT("freejoint");
			Inertial |= C->Tag == TEXT("inertial");
		}
		if (Joint && !Inertial)
		{
			Error = TEXT("body '") + Node.Get(TEXT("name")) +
			        TEXT("' requires explicit <inertial>; robot inertia must come from MJCF");
			return false;
		}
		if (!Joint && !Inertial)
			for (auto &C : Node.Children)
				if (C->Tag == TEXT("geom"))
				{
					Package.Warnings.Add(TEXT("fixed body '") + Node.Get(TEXT("name")) +
					                     TEXT("' has geoms but no explicit inertia; it contributes no mass"));
					break;
				}
	}
	if (Node.Tag == TEXT("geom"))
	{
		FString Class = Node.Attributes.Contains(TEXT("class")) ? Node.Get(TEXT("class")) : InheritedClass;
		TMap<FString, FString> Effective;
		if (const auto *D = Defs.Find(Class))
			Effective = *D;
		// An explicit orientation replaces the alternative inherited from defaults.
		const TArray<FString> OrientationKeys = {TEXT("quat"), TEXT("axisangle"), TEXT("euler"),
		                                         TEXT("xyaxes"), TEXT("zaxis")};
		if (OrientationKeys.ContainsByPredicate(
		        [&](const FString &Key) { return Node.Attributes.Contains(Key); }))
			for (const auto &Key : OrientationKeys)
				Effective.Remove(Key);
		for (auto &A : Node.Attributes)
			Effective.Add(A.Key, A.Value);
		const FString *MeshName = Effective.Find(TEXT("mesh"));
		const FMesh *Mesh = MeshName ? Meshes.Find(*MeshName) : nullptr;
		if (Mesh)
		{
			for (const TCHAR *Key : {TEXT("contype"), TEXT("conaffinity")})
			{
				const FString *Value = Effective.Find(Key);
				int32 Mask = -1;
				if (!Value || !LexTryParseString(Mask, **Value) || Mask != 0)
				{
					Error = TEXT("GLB visual geom '") + Node.Get(TEXT("name")) + TEXT("' requires ") + Key +
					        TEXT("=\"0\"; define collision separately in MJCF");
					return false;
				}
			}
			if (const auto *Mass = Effective.Find(TEXT("mass")))
			{
				double V;
				if (!LexTryParseString(V, **Mass) || V != 0)
				{
					Error = TEXT("GLB visual geom must have mass=0 or omit mass; use explicit body inertia");
					return false;
				}
			}
			if (Effective.Contains(TEXT("fromto")))
			{
				Error = TEXT("GLB visuals do not support fromto; use pos and orientation");
				return false;
			}
			URSoccerLab::FExternalRobotVisual Visual;
			Visual.File = Mesh->File;
			Visual.Scale = Mesh->Scale;
			Visual.bQuaternionPose = Effective.Contains(TEXT("quat")) ||
			                         !OrientationKeys.ContainsByPredicate(
			                             [&](const FString &Key) { return Effective.Contains(Key); });
			Visual.SiteName = FString::Printf(TEXT("__urs_visual_%d"), Package.Visuals.Num());
			Package.Visuals.Add(Visual);
			// Sites have no collision or inertia. MuJoCo resolves the visual pose,
			// including nested frames/default orientations, before we remove them.
			Node.Tag = TEXT("site");
			Node.Attributes.Empty();
			Node.Children.Empty();
			Node.Attributes.Add(TEXT("name"), Visual.SiteName);
			Node.Attributes.Add(TEXT("size"), TEXT("0.001"));
			// Visual metadata must not inherit an unrelated site pose.
			Node.Attributes.Add(TEXT("pos"), TEXT("0 0 0"));
			if (!OrientationKeys.ContainsByPredicate(
			        [&](const FString &Key) { return Effective.Contains(Key); }))
				Node.Attributes.Add(TEXT("quat"), TEXT("1 0 0 0"));

			for (const TCHAR *Key :
			     {TEXT("pos"), TEXT("quat"), TEXT("axisangle"), TEXT("euler"), TEXT("xyaxes"), TEXT("zaxis")})
				if (const auto *V = Effective.Find(Key))
					Node.Attributes.Add(Key, *V);
		}
	}
	for (int32 I = Node.Children.Num() - 1; I >= 0; --I)
	{
		auto &C = Node.Children[I];
		if (Node.Tag == TEXT("asset") && C->Tag == TEXT("mesh") && Meshes.Contains(C->Get(TEXT("name"))))
		{
			Node.Children.RemoveAt(I);
			continue;
		}
		if (!Transform(*C, InheritedClass, Meshes, Defs, Package, Error))
			return false;
	}
	return true;
}
template <typename T> T *Component(AURSExternalRobot *Actor, const FString &Name, USceneComponent *Parent)
{
	const FString ObjectName = T::StaticClass()->GetName() + TEXT("_") + Name;
	T *C = NewObject<T>(Actor, MakeUniqueObjectName(Actor, T::StaticClass(), FName(*ObjectName)));
	Actor->AddInstanceComponent(C);
	C->SetupAttachment(Parent);
	if constexpr (TIsDerivedFrom<T, UMjComponent>::IsDerived)
		C->MjName = Name;
	return C;
}
} // namespace

namespace URSoccerLab
{
FExternalRobotPackage::~FExternalRobotPackage()
{
	if (Model)
		mj_deleteModel(Model);
}
TSharedPtr<FExternalRobotPackage> FExternalRobotLoader::Load(const FString &ManifestPath, FString &Error)
{
	Error.Reset();
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *ManifestPath))
	{
		Error = TEXT("cannot read robot manifest: ") + ManifestPath;
		return nullptr;
	}
	TSharedPtr<FJsonObject> Manifest;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Manifest) || !Manifest)
	{
		Error = TEXT("invalid robot manifest JSON");
		return nullptr;
	}
	auto Package = MakeShared<FExternalRobotPackage>();
	FString Version, ModelFile;
	const TSharedPtr<FJsonObject> *Bindings = nullptr;
	if (!Manifest->TryGetStringField(TEXT("version"), Version) || Version != TEXT("urs_robot_v1") ||
	    !Manifest->TryGetStringField(TEXT("id"), Package->Id) || Package->Id.IsEmpty() ||
	    !Manifest->TryGetStringField(TEXT("model"), ModelFile) || ModelFile.IsEmpty() ||
	    !Manifest->TryGetNumberField(TEXT("default_base_height_m"), Package->DefaultBaseHeightM) ||
	    !FMath::IsFinite(Package->DefaultBaseHeightM) ||
	    !Manifest->TryGetObjectField(TEXT("bindings"), Bindings) ||
	    !(*Bindings)->TryGetStringField(TEXT("base_body"), Package->BaseBody) ||
	    !(*Bindings)->TryGetStringField(TEXT("head_body"), Package->HeadBody) ||
	    !(*Bindings)->TryGetStringField(TEXT("left_camera"), Package->LeftCamera) ||
	    !(*Bindings)->TryGetStringField(TEXT("right_camera"), Package->RightCamera))
	{
		Error = TEXT("robot manifest requires urs_robot_v1, id, model, finite default_base_height_m and "
		             "base/head/left/right bindings");
		return nullptr;
	}
	const FString XmlPath = Absolute(FPaths::GetPath(ManifestPath), ModelFile);
	FXmlFile Xml(XmlPath);
	if (!Xml.IsValid())
	{
		Error = TEXT("invalid robot MJCF: ") + XmlPath;
		return nullptr;
	}
	TSet<FString> Stack;
	Stack.Add(XmlPath);
	auto Root = Expand(Xml.GetRootNode(), FPaths::GetPath(XmlPath), Stack, Error);
	if (!Root)
		return nullptr;
	if (Root->Tag != TEXT("mujoco"))
	{
		Error = TEXT("robot model root must be <mujoco>");
		return nullptr;
	}
	// Includes splice compiler settings into document order; later attributes win.
	auto Compiler = MakeShared<FNode>();
	Compiler->Tag = TEXT("compiler");
	for (int32 I = 0; I < Root->Children.Num(); ++I)
	{
		const auto &C = Root->Children[I];
		if (C->Tag != TEXT("compiler"))
			continue;
		for (const auto &A : C->Attributes)
			Compiler->Attributes.Add(A.Key, A.Value);
		Compiler->Children.Append(C->Children);
		Root->Children.RemoveAt(I--);
	}
	Root->Children.Insert(Compiler, 0);
	if (Compiler->Get(TEXT("inertiafromgeom")) == TEXT("true"))
	{
		Error = TEXT("robot compiler inertiafromgeom=true is incompatible with explicit MJCF inertia");
		return nullptr;
	}
	Compiler->Attributes.Add(TEXT("inertiafromgeom"), TEXT("false"));
	const FString Directory = FPaths::GetPath(XmlPath);
	FString MeshDir = Compiler->Get(TEXT("meshdir")), TextureDir = Compiler->Get(TEXT("texturedir"));
	if (!Compiler->Get(TEXT("assetdir")).IsEmpty())
		MeshDir = TextureDir = Compiler->Get(TEXT("assetdir"));
	MeshDir = Absolute(Directory, MeshDir);
	TextureDir = Absolute(Directory, TextureDir);
	Compiler->Attributes.Remove(TEXT("assetdir"));
	Compiler->Attributes.Remove(TEXT("meshdir"));
	Compiler->Attributes.Remove(TEXT("texturedir"));
	Compiler->Attributes.Add(TEXT("strippath"), TEXT("false"));
	TMap<FString, FMesh> Meshes;
	for (auto &Section : Root->Children)
		if (Section->Tag == TEXT("asset"))
			for (auto &Asset : Section->Children)
			{
				if (Asset->Tag != TEXT("mesh") && Asset->Tag != TEXT("texture") &&
				    Asset->Tag != TEXT("hfield") && Asset->Tag != TEXT("skin"))
					continue;
				FString File = Asset->Get(TEXT("file"));
				if (File.IsEmpty())
					continue;
				File = Absolute((Asset->Tag == TEXT("mesh") || Asset->Tag == TEXT("skin"))
				                    ? MeshDir
				                    : (Asset->Tag == TEXT("texture") ? TextureDir : Directory),
				                File);
				Asset->Attributes.Add(TEXT("file"), File);
				if (Asset->Tag == TEXT("mesh") &&
				    FPaths::GetExtension(File).Equals(TEXT("glb"), ESearchCase::IgnoreCase))
				{
					FString Name = Asset->Get(TEXT("name"));
					if (Name.IsEmpty())
						Name = FPaths::GetBaseFilename(File);
					if (Meshes.Contains(Name))
					{
						Error = TEXT("duplicate GLB mesh name: ") + Name;
						return nullptr;
					}
					FMesh Mesh;
					Mesh.File = File;
					if (!FPaths::FileExists(File))
					{
						Error = TEXT("missing robot GLB: ") + File;
						return nullptr;
					}
					if (Asset->Attributes.Contains(TEXT("scale")))
					{
						TArray<double> V;
						if (!Numbers(Asset->Get(TEXT("scale")), 3, V) || V[0] <= 0 || V[1] <= 0 || V[2] <= 0)
						{
							Error = TEXT("GLB mesh scale requires three positive finite numbers");
							return nullptr;
						}
						// MuJoCo -> Unreal flips Y, without exchanging scale axes.
						Mesh.Scale = FVector(V[0], V[1], V[2]);
					}
					for (const TCHAR *Unsupported : {TEXT("refpos"), TEXT("refquat")})
						if (Asset->Attributes.Contains(Unsupported))
						{
							Error = TEXT(
							    "GLB mesh refpos/refquat are unsupported; put visual placement on its geom");
							return nullptr;
						}
					Asset->Attributes.Add(TEXT("name"), Name);
					Meshes.Add(Name, Mesh);
				}
			}
	TMap<FString, TMap<FString, FString>> Defs;
	Defaults(*Root, {}, Defs);
	if (!Transform(*Root, TEXT(""), Meshes, Defs, *Package, Error))
		return nullptr;
	// Remove GLB references from geom defaults as well, after resolving all visuals.
	TFunction<void(FNode &)> CleanDefaults = [&](FNode &N) {
		if (N.Tag == TEXT("default"))
			for (auto &C : N.Children)
				if (C->Tag == TEXT("geom"))
					if (Meshes.Contains(C->Get(TEXT("mesh"))))
					{
						C->Attributes.Remove(TEXT("mesh"));
						if (C->Get(TEXT("type")) == TEXT("mesh"))
							C->Attributes.Remove(TEXT("type"));
					}
		for (auto &C : N.Children)
			CleanDefaults(*C);
	};
	CleanDefaults(*Root);
	Package->PhysicsXml = Serialize(*Root);
	char Message[2048] = {};
	mjSpec *Spec = mj_parseXMLString(TCHAR_TO_UTF8(*Package->PhysicsXml), nullptr, Message, sizeof(Message));
	if (!Spec)
	{
		Error = FString(TEXT("robot MJCF parse failed: ")) + UTF8_TO_TCHAR(Message);
		return nullptr;
	}
	// Metadata sites must not acquire pose/fromto settings from physical site defaults.
	mjsSite Neutral = {};
	mjs_defaultSite(&Neutral);
	for (const auto &Visual : Package->Visuals)
	{
		mjsSite *Site = mjs_asSite(mjs_findElement(Spec, mjOBJ_SITE, TCHAR_TO_UTF8(*Visual.SiteName)));
		if (!Site)
		{
			Error = TEXT("missing temporary visual site");
			mj_deleteSpec(Spec);
			return nullptr;
		}
		mju_copy(Site->fromto, Neutral.fromto, 6);
		if (Visual.bQuaternionPose)
			Site->alt = Neutral.alt;
	}
	Package->Model = mj_compile(Spec, nullptr);
	if (!Package->Model)
		Error = FString(TEXT("robot MJCF compile failed: ")) + UTF8_TO_TCHAR(mjs_getError(Spec));
	mj_deleteSpec(Spec);
	if (!Package->Model)
		return nullptr;
	const mjModel *M = Package->Model;
	for (const auto &Binding : {TPair<mjtObj, FString>(mjOBJ_BODY, Package->BaseBody),
	                            {mjOBJ_BODY, Package->HeadBody},
	                            {mjOBJ_CAMERA, Package->LeftCamera},
	                            {mjOBJ_CAMERA, Package->RightCamera}})
		if (Binding.Value.IsEmpty() || mj_name2id(M, Binding.Key, TCHAR_TO_UTF8(*Binding.Value)) < 0)
		{
			Error = TEXT("unresolved robot binding: ") + Binding.Value;
			return nullptr;
		}
	const int Base = mj_name2id(M, mjOBJ_BODY, TCHAR_TO_UTF8(*Package->BaseBody));
	const int Head = mj_name2id(M, mjOBJ_BODY, TCHAR_TO_UTF8(*Package->HeadBody));
	if (Base <= 0 || M->body_rootid[Base] != Base || M->body_rootid[Head] != Base)
	{
		Error = TEXT("base_body must be a robot root and head_body must belong to that robot");
		return nullptr;
	}
	for (const auto &Name : {Package->LeftCamera, Package->RightCamera})
	{
		const int Camera = mj_name2id(M, mjOBJ_CAMERA, TCHAR_TO_UTF8(*Name));
		if (M->body_rootid[M->cam_bodyid[Camera]] != Base || M->cam_resolution[Camera * 2] != 640 ||
		    M->cam_resolution[Camera * 2 + 1] != 480)
		{
			Error = TEXT("bound robot cameras must belong to base_body and use 640x480 resolution");
			return nullptr;
		}
		if (M->cam_mode[Camera] != mjCAMLIGHT_FIXED || M->cam_projection[Camera] != mjPROJ_PERSPECTIVE)
		{
			Error = TEXT("robot camera bindings require fixed perspective cameras");
			return nullptr;
		}
	}
	if (Package->LeftCamera == Package->RightCamera)
	{
		Error = TEXT("left and right camera bindings must differ");
		return nullptr;
	}
	int CollisionCount = 0;
	for (int G = 0; G < M->ngeom; ++G)
		if (M->body_rootid[M->geom_bodyid[G]] == Base && (M->geom_contype[G] || M->geom_conaffinity[G]))
			++CollisionCount;
	if (!CollisionCount)
		Package->Warnings.Add(TEXT("robot MJCF contains no collision-enabled geoms"));
	if (Package->Visuals.IsEmpty())
		Package->Warnings.Add(TEXT("robot MJCF contains no GLB visuals"));
	return Package;
}

bool FExternalRobotLoader::Populate(AURSExternalRobot *Robot, const FExternalRobotPackage &Package,
                                    FString &Error)
{
	Robot->PhysicsXml = Package.PhysicsXml;
	Robot->BaseBodyName = Package.BaseBody;
	Robot->HeadBodyName = Package.HeadBody;
	Robot->LeftCameraName = Package.LeftCamera;
	Robot->RightCameraName = Package.RightCamera;
	const mjModel *M = Package.Model;
	mjData *D = mj_makeData(M);
	if (!D)
	{
		Error = TEXT("cannot allocate robot preview state");
		return false;
	}
	mj_forward(M, D);
	TArray<UMjBody *> Bodies;
	Bodies.SetNumZeroed(M->nbody);
	for (int B = 1; B < M->nbody; ++B)
	{
		const char *N = mj_id2name(M, mjOBJ_BODY, B);
		if (!N)
		{
			Error = TEXT("external robot bodies must be named");
			mj_deleteData(D);
			return false;
		}
		Bodies[B] = Component<UMjBody>(Robot, UTF8_TO_TCHAR(N), Robot->DefaultSceneRoot);
		Bodies[B]->SetRelativeTransform(
		    FTransform(MjUtils::MjToUERotation(D->xquat + B * 4), MjUtils::MjToUEPosition(D->xpos + B * 3)));
		Bodies[B]->RegisterComponent();
	}
	for (int J = 0; J < M->njnt; ++J)
	{
		const char *N = mj_id2name(M, mjOBJ_JOINT, J);
		if (!N)
		{
			Error = TEXT("external robot joints must be named");
			mj_deleteData(D);
			return false;
		}
		auto *C = Component<UMjJoint>(Robot, UTF8_TO_TCHAR(N), Bodies[M->jnt_bodyid[J]]);
		C->bOverride_Type = true;
		C->Type = static_cast<EMjJointType>(M->jnt_type[J]);
		C->RegisterComponent();
	}
	for (int A = 0; A < M->nu; ++A)
	{
		const char *N = mj_id2name(M, mjOBJ_ACTUATOR, A);
		if (!N)
		{
			Error = TEXT("external robot actuators must be named");
			mj_deleteData(D);
			return false;
		}
		Component<UMjActuator>(Robot, UTF8_TO_TCHAR(N), Robot->DefaultSceneRoot)->RegisterComponent();
	}
	for (int C = 0; C < M->ncam; ++C)
	{
		const char *N = mj_id2name(M, mjOBJ_CAMERA, C);
		if (!N || !Bodies.IsValidIndex(M->cam_bodyid[C]) || !Bodies[M->cam_bodyid[C]])
		{
			Error = TEXT("robot cameras must be named and attached to a body");
			mj_deleteData(D);
			return false;
		}
		auto *Camera = Component<UMjCamera>(Robot, UTF8_TO_TCHAR(N), Bodies[M->cam_bodyid[C]]);
		Camera->bOverride_Pos = Camera->bOverride_Quat = true;
		Camera->Pos = MjUtils::MjToUEPosition(M->cam_pos + C * 3);
		Camera->Quat = MjUtils::MjToUERotation(M->cam_quat + C * 4);
		Camera->SetRelativeTransform(FTransform(Camera->Quat, Camera->Pos));
		Camera->bOverride_fovy = true;
		Camera->fovy = M->cam_fovy[C];
		Camera->bOverride_resolution = true;
		Camera->resolution = {M->cam_resolution[C * 2], M->cam_resolution[C * 2 + 1]};
		Camera->RegisterComponent();
		// Nested default subobjects are not registered automatically for NewObject cameras.
		Camera->CaptureComponent->RegisterComponent();
	}

	for (const auto &V : Package.Visuals)
	{
		int Site = mj_name2id(M, mjOBJ_SITE, TCHAR_TO_UTF8(*V.SiteName));
		if (Site < 0 || !Bodies[M->site_bodyid[Site]])
		{
			Error = TEXT("GLB visual must attach to a robot body");
			mj_deleteData(D);
			return false;
		}
		auto &CachedMesh = Package.LoadedMeshes.FindOrAdd(V.File);
		UStaticMesh *Mesh = CachedMesh.Get();
		if (!Mesh)
		{
			FglTFRuntimeConfig Config;
			// Match Interchange: GLB X -> UE X, Y -> UE Z, Z -> UE Y.
			Config.TransformBaseType = EglTFRuntimeTransformBaseType::YForward;
			UglTFRuntimeAsset *Asset =
			    UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(V.File, false, Config);
			if (Asset)
			{
				// The editor importer supplied a tangent basis even for UV-less
				// meshes. glTFRuntime otherwise leaves both tangent axes zero.
				// Keep authored tangents and UV-based generation unchanged.
				const auto Parser = Asset->GetParser();
				const FDelegateHandle TangentFallback = FglTFRuntimeParser::OnLoadedPrimitive.AddLambda(
				    [Parser](TSharedRef<FglTFRuntimeParser> LoadedParser, TSharedRef<FJsonObject>,
				             FglTFRuntimePrimitive& Primitive)
				    {
					    if (LoadedParser != Parser) return;
					    // A glTF primitive without a material uses the standard white,
					    // rough metallic default, rather than Unreal's WorldGrid shader.
					    if (!Primitive.bHasMaterial)
					    {
						    FglTFRuntimeMaterial Default;
						    Default.bHasBaseColorFactor = true;
						    Default.BaseColorFactor = FLinearColor::White;
						    Default.bHasMetallicFactor = Default.bHasRoughnessFactor = true;
						    Default.MetallicFactor = Default.RoughnessFactor = 1;
						    Default.BaseSpecularFactor = 1;
						    Primitive.MaterialName = TEXT("glTF_default");
						    Primitive.Material = Parser->BuildMaterial(INDEX_NONE, Primitive.MaterialName,
						        Default, FglTFRuntimeMaterialsConfig(), !Primitive.Colors.IsEmpty());
						    Primitive.bHasMaterial = true;
					    }
					    if (!Primitive.UVs.IsEmpty() ||
					        !Primitive.Tangents.IsEmpty() || Primitive.Normals.Num() != Primitive.Positions.Num())
						    return;
					    for (const FVector& Normal : Primitive.Normals)
					    {
						    FVector Tangent, Bitangent;
						    Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector).FindBestAxisVectors(Tangent, Bitangent);
						    Primitive.Tangents.Add(FVector4(Tangent, 1));
					    }
				    });
				FglTFRuntimeStaticMeshConfig MeshConfig;
				MeshConfig.Outer = GetTransientPackage();
				MeshConfig.bBuildLumenCards = true;
				Mesh = Asset->LoadStaticMeshRecursive(TEXT(""), {}, MeshConfig);
				FglTFRuntimeParser::OnLoadedPrimitive.Remove(TangentFallback);
				if (!Asset->GetErrors().IsEmpty())
				{
					Error = TEXT("robot GLB import failed: ") + V.File + TEXT(": ") +
					        FString::Join(Asset->GetErrors(), TEXT("; "));
					mj_deleteData(D);
					return false;
				}
				CachedMesh = Mesh;
			}
			if (!Mesh)
			{
				Error = TEXT("failed to load robot GLB: ") + V.File;
				mj_deleteData(D);
				return false;
			}
		}
		auto *Visual = Component<UStaticMeshComponent>(Robot, V.SiteName, Bodies[M->site_bodyid[Site]]);
		Visual->bDisallowNanite = true;
		Visual->SetMobility(EComponentMobility::Movable);
		Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Visual->SetStaticMesh(Mesh);
		Visual->SetRelativeTransform(FTransform(MjUtils::MjToUERotation(M->site_quat + Site * 4),
		                                        MjUtils::MjToUEPosition(M->site_pos + Site * 3), V.Scale));
		Visual->RegisterComponent();
	}
	mj_deleteData(D);
	return true;
}
} // namespace URSoccerLab

void AURSExternalRobot::Setup(mjSpec *Spec, mjVFS *VFS)
{
	m_spec = Spec;
	m_vfs = VFS;
	m_prefix = GetName() + TEXT("_");
	bAttachFailed = false;
	if (m_wrapper)
	{
		delete m_wrapper;
		m_wrapper = nullptr;
	}
	char Error[2048] = {};
	mjSpec *Child = mj_parseXMLString(TCHAR_TO_UTF8(*PhysicsXml), nullptr, Error, sizeof(Error));
	if (!Child)
	{
		bAttachFailed = true;
		UE_LOG(LogTemp, Error, TEXT("External robot parse: %hs"), Error);
		return;
	}
	// These temporary sites were used only to resolve visual transforms.
	for (mjsElement *E = mjs_firstElement(Child, mjOBJ_SITE); E;)
	{
		mjsElement *Next = mjs_nextElement(Child, E);
		if (FString(UTF8_TO_TCHAR(mjs_getString(mjs_getName(E)))).StartsWith(TEXT("__urs_visual_")))
			mjs_delete(Child, E);
		E = Next;
	}
	// External physics assets are filesystem inputs, never converted cooked meshes.
	mjsFrame *Frame = mjs_addFrame(mjs_findBody(Spec, "world"), nullptr);
	MjUtils::UEToMjPosition(GetActorLocation(), Frame->pos);
	MjUtils::UEToMjRotation(GetActorQuat(), Frame->quat);
	if (!mjs_attach(Frame->element, Child->element, TCHAR_TO_UTF8(*m_prefix), ""))
	{
		bAttachFailed = true;
		UE_LOG(LogTemp, Error, TEXT("External robot attach: %hs"), mjs_getError(Spec));
	}
	mj_deleteSpec(Child);
}

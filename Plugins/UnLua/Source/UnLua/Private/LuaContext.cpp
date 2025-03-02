// Tencent is pleased to support the open source community by making UnLua available.
// 
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the MIT License (the "License"); 
// you may not use this file except in compliance with the License. You may obtain a copy of the License at
//
// http://opensource.org/licenses/MIT
//
// Unless required by applicable law or agreed to in writing, 
// software distributed under the License is distributed on an "AS IS" BASIS, 
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
// See the License for the specific language governing permissions and limitations under the License.

#include "LuaContext.h"
#include "LuaCore.h"
#include "LuaDynamicBinding.h"
#include "UnLuaEx.h"
#include "UnLuaManager.h"
#include "UnLuaInterface.h"
#include "UnLuaDelegates.h"
#include "UnLuaDebugBase.h"
#include "UEObjectReferencer.h"
#include "CollisionHelper.h"
#include "DelegateHelper.h"
#include "ReflectionUtils/PropertyCreator.h"
#include "DefaultParamCollection.h"
#include "ReflectionUtils/ReflectionRegistry.h"

// ADD_LuaPanda
#include "LibLuasocket.h"
#include "LuaPanda.h"

#if WITH_EDITOR
#include "Editor.h"
#endif


/**
 * Statically exported callback for 'Hotfix'
 */
bool OnModuleHotfixed(const char* ModuleName)
{
    if (!GLuaCxt->IsEnable() || !ModuleName)
    {
        UE_LOG(LogUnLua, Log, TEXT("%s: Invalid module name!"), ANSI_TO_TCHAR(__FUNCTION__));
        return false;
    }

    bool bSuccess = GLuaCxt->GetUnLuaManager()->OnModuleHotfixed(UTF8_TO_TCHAR(ModuleName));
#if !UE_BUILD_SHIPPING
    if (!bSuccess)
    {
        UE_LOG(LogUnLua, Log, TEXT("%s: Failed to update module!"), ANSI_TO_TCHAR(__FUNCTION__));
    }
#endif
    return bSuccess;
}

EXPORT_FUNCTION(bool, OnModuleHotfixed, const char*)


FLuaContext* GLuaCxt = nullptr;

/**
 * Create GLuaCxt
 */
FLuaContext* FLuaContext::Create()
{
    if (!GLuaCxt)
    {
        static FLuaContext Context;
        GLuaCxt = &Context;
    }
    return GLuaCxt;
}

/**
 * Register different engine delegates
 */
void FLuaContext::RegisterDelegates()
{
#if SUPPORTS_COMMANDLET == 0
    if (IsRunningCommandlet())
    {
        return;
    }
#endif

    FWorldDelegates::OnWorldCleanup.AddRaw(this, &FLuaContext::OnWorldCleanup);
    FCoreDelegates::OnPostEngineInit.AddRaw(this, &FLuaContext::OnPostEngineInit);   // called before FCoreDelegates::OnFEngineLoopInitComplete.Broadcast(), after GEngine->Init(...)
    FCoreDelegates::OnPreExit.AddRaw(this, &FLuaContext::OnPreExit);                 // called before StaticExit()
    FCoreDelegates::OnAsyncLoadingFlushUpdate.AddRaw(this, &FLuaContext::OnAsyncLoadingFlushUpdate);
    FCoreDelegates::OnHandleSystemError.AddRaw(this, &FLuaContext::OnCrash);
    FCoreDelegates::OnHandleSystemEnsure.AddRaw(this, &FLuaContext::OnCrash);
    FCoreUObjectDelegates::PostLoadMapWithWorld.AddRaw(this, &FLuaContext::PostLoadMapWithWorld);
    //FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddRaw(this, &FLuaContext::OnPreGarbageCollect);

#if WITH_EDITOR
    FEditorDelegates::PreBeginPIE.AddRaw(this, &FLuaContext::PreBeginPIE);
    FEditorDelegates::PostPIEStarted.AddRaw(this, &FLuaContext::PostPIEStarted);
    FEditorDelegates::PrePIEEnded.AddRaw(this, &FLuaContext::PrePIEEnded);
#endif

    GUObjectArray.AddUObjectCreateListener(this);    // add listener for creating UObject
    GUObjectArray.AddUObjectDeleteListener(this);    // add listener for deleting UObject
}

/**
 * Create Lua state (main thread) and register/create base libs/tables/classes
 */
void FLuaContext::CreateState()
{
#if SUPPORTS_COMMANDLET == 0
    if (IsRunningCommandlet())
    {
        return;
    }
#endif

    if (!L)
    {

        L = lua_newstate(FLuaContext::LuaAllocator, nullptr);       // create main Lua thread
        check(L);
        luaL_openlibs(L);                                           // open all standard Lua libraries

        AddSearcher(LoadFromCustomLoader, 2);
        AddSearcher(LoadFromFileSystem, 3);
        AddSearcher(LoadFromBuiltinLibs, 4);

        lua_pushstring(L, "ObjectMap");                             // create weak table 'ObjectMap'
        CreateWeakValueTable(L);
        lua_rawset(L, LUA_REGISTRYINDEX);

        lua_pushstring(L, "StructMap");                             // create weak table 'StructMap'
        CreateWeakValueTable(L);
        lua_rawset(L, LUA_REGISTRYINDEX);

        lua_pushstring(L, "ScriptContainerMap");                    // create weak table 'ScriptContainerMap'
        CreateWeakValueTable(L);
        lua_rawset(L, LUA_REGISTRYINDEX);

        lua_pushstring(L, "ArrayMap");                              // create weak table 'ArrayMap'
        CreateWeakValueTable(L);
        lua_rawset(L, LUA_REGISTRYINDEX);

        CreateNamespaceForUE(L);                                    // create 'UE' namespace (table)

        // register global Lua functions
        lua_register(L, "RegisterEnum", Global_RegisterEnum);
        lua_register(L, "RegisterClass", Global_RegisterClass);
        lua_register(L, "GetUProperty", Global_GetUProperty);
        lua_register(L, "SetUProperty", Global_SetUProperty);
        lua_register(L, "LoadObject", Global_LoadObject);
        lua_register(L, "LoadClass", Global_LoadClass);
        lua_register(L, "NewObject", Global_NewObject);
        lua_register(L, "UnLua_AddToClassWhiteSet", Global_AddToClassWhiteSet);
        lua_register(L, "UnLua_RemoveFromClassWhiteSet", Global_RemoveFromClassWhiteSet);
        lua_register(L, "UnLua_UnRegisterClass", Global_UnRegisterClass);

        lua_register(L, "UEPrint", Global_Print);

        // register collision related enums
        FCollisionHelper::Initialize();     // initialize collision helper stuff
        RegisterECollisionChannel(L);
        RegisterEObjectTypeQuery(L);
        RegisterETraceTypeQuery(L);

        if (FUnLuaDelegates::ConfigureLuaGC.IsBound())
        {
            FUnLuaDelegates::ConfigureLuaGC.Execute(L);
        }
        else
        {
#if 504 == LUA_VERSION_NUM
            lua_gc(L, LUA_GCGEN);
#else
            // default Lua GC config in UnLua
            lua_gc(L, LUA_GCSETPAUSE, 100);
            lua_gc(L, LUA_GCSETSTEPMUL, 5000);
#endif
        }

        // add new package path
        FString LuaSrcPath = GLuaSrcFullPath + TEXT("?.lua");
        AddPackagePath(L, TCHAR_TO_UTF8(*LuaSrcPath));

        FUnLuaDelegates::OnPreStaticallyExport.Broadcast();

        RegisterClass(L, "UClass", "UObject");                      // register base class

        // register statically exported classes
        for (TMap<FName, UnLua::IExportedClass*>::TIterator It(ExportedNonReflectedClasses); It; ++It)
        {
            It.Value()->Register(L);
        }

        // register statically exported global functions
        for (UnLua::IExportedFunction* Function : ExportedFunctions)
        {
            Function->Register(L);
        }

        // register statically exported enums
        for (UnLua::IExportedEnum* Enum : ExportedEnums)
        {
            Enum->Register(L);
        }

// ADD_LuaPanda
		if (FLibLuasocketModule::IsAvailable())
		{
			FLibLuasocketModule::Get().SetupLuasocket(L);
			lua_pushboolean(L, true);
			lua_setglobal(L, "SupportLuasocket");

			UE_LOG(LogUnLua, Display, TEXT("Lua state setup with Luasocket."));

			if (UE_BUILD_SHIPPING == 0 && FLuaPanda::IsAvailable())
			{
				FLuaPanda::Get().SetupLuaPanda(L);
				lua_pushboolean(L, true);
				lua_setglobal(L, "SupportLuaPanda");
				UE_LOG(LogUnLua, Display, TEXT("Lua state setup with LuaPanda."));
			}
		}
        
        FUnLuaDelegates::OnLuaStateCreated.Broadcast(L);
    }
}

/**
 * Enable UnLua
 */
void FLuaContext::SetEnable(bool InEnable)
{
    if (InEnable)
    {
        Initialize();
    }
    else
    {
        Cleanup(true);
    }
}

/**
 * Is UnLua enabled?
 */
bool FLuaContext::IsEnable() const
{
    return L && bEnable;
}

/**
 * Statically export a global functions
 */
bool FLuaContext::ExportFunction(UnLua::IExportedFunction* Function)
{
    if (Function)
    {
        ExportedFunctions.Add(Function);
        return true;
    }
    return false;
}

/**
 * Statically export an enum
 */
bool FLuaContext::ExportEnum(UnLua::IExportedEnum* Enum)
{
    if (Enum)
    {
        ExportedEnums.Add(Enum);
        return true;
    }
    return false;
}

/**
 * Statically export a class
 */
bool FLuaContext::ExportClass(UnLua::IExportedClass* Class)
{
    if (Class)
    {
        TMap<FName, UnLua::IExportedClass*>& ExportedClasses = Class->IsReflected() ? ExportedReflectedClasses : ExportedNonReflectedClasses;
        ExportedClasses.Add(Class->GetName(), Class);
        return true;
    }
    return false;
}

/**
 * Find a statically exported class
 */
UnLua::IExportedClass* FLuaContext::FindExportedClass(FName Name)
{
    UnLua::IExportedClass** Class = ExportedReflectedClasses.Find(Name);
    if (Class)
    {
        return *Class;
    }
    Class = ExportedNonReflectedClasses.Find(Name);
    return Class ? *Class : nullptr;
}

/**
 * Find a statically exported reflected class
 */
UnLua::IExportedClass* FLuaContext::FindExportedReflectedClass(FName Name)
{
    UnLua::IExportedClass** Class = ExportedReflectedClasses.Find(Name);
    return Class ? *Class : nullptr;
}

/**
 * Find a statically exported non reflected class
 */
UnLua::IExportedClass* FLuaContext::FindExportedNonReflectedClass(FName Name)
{
    UnLua::IExportedClass** Class = ExportedNonReflectedClasses.Find(Name);
    return Class ? *Class : nullptr;
}

/**
 * Add a type interface
 */
bool FLuaContext::AddTypeInterface(FName Name, TSharedPtr<UnLua::ITypeInterface> TypeInterface)
{
    if (Name == NAME_None || !TypeInterface)
    {
        return false;
    }

    TSharedPtr<UnLua::ITypeInterface>* TypeInterfacePtr = TypeInterfaces.Find(Name);
    if (!TypeInterfacePtr)
    {
        TypeInterfaces.Add(Name, TypeInterface);
    }
    return true;
}

/**
 * Find a type interface
 */
TSharedPtr<UnLua::ITypeInterface> FLuaContext::FindTypeInterface(FName Name)
{
    TSharedPtr<UnLua::ITypeInterface>* TypeInterfacePtr = TypeInterfaces.Find(Name);
    return TypeInterfacePtr ? *TypeInterfacePtr : TSharedPtr<UnLua::ITypeInterface>();
}

/**
 * Try to bind Lua module for a UObject
 */
bool FLuaContext::TryToBindLua(UObject* Object)
{
    if (!bEnable || !IsUObjectValid(Object))
        return false;

    const bool bIsCDO = Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject);
    if (bIsCDO)
    {
        // filter out class default and template objects
        return false;
    }

    UClass* Class = Object->GetClass();
    if (Class->IsChildOf<UPackage>() || Class->IsChildOf<UClass>() || Class->HasAnyClassFlags(CLASS_NewerVersionExists))
    {
        // filter out UPackage and UClass and recompiled objects
        return false;
    }

    if (!IsInGameThread() || IsAsyncLoading())
    {
        // all bind operation should be in game thread, include dynamic bind
        FScopeLock Lock(&Async2MainCS);
        Candidates.AddUnique(Object);
        return false;
    }

    static UClass* InterfaceClass = UUnLuaInterface::StaticClass();

    if (!Class->ImplementsInterface(InterfaceClass))
    {
        // dynamic binding
        if (!GLuaDynamicBinding.IsValid(Class))
            return false;

        return Manager->Bind(Object, Class, *GLuaDynamicBinding.ModuleName, GLuaDynamicBinding.InitializerTableRef);
    }

    // filter some object in bp nest case
    // RF_WasLoaded & RF_NeedPostLoad?
    UObject* Outer = Object->GetOuter();
    if (Outer
        && Outer->GetFName().IsEqual("WidgetTree")
        && Object->HasAllFlags(RF_NeedInitialization | RF_NeedPostLoad | RF_NeedPostLoadSubobjects))
    {
        return false;
    }

    if (GWorld)
    {
        FString ObjectName;
        Object->GetFullName(GWorld, ObjectName);
        if (ObjectName.Contains(".WidgetArchetype:") || ObjectName.Contains(":WidgetTree."))
        {
            UE_LOG(LogUnLua, Warning, TEXT("Filter UObject of %s in WidgetArchetype"), *ObjectName);
            return false;
        }
    }

    UFunction* Func = Class->FindFunctionByName(FName("GetModuleName")); // find UFunction 'GetModuleName'. hard coded!!!
    if (!Func)
        return false;

    // native func may not be bind in level bp
    if (!Func->GetNativeFunc())
    {
        Func->Bind();
        if (!Func->GetNativeFunc())
        {
            UE_LOG(LogUnLua, Warning, TEXT("TryToBindLua: bind native function failed for GetModuleName in object %s"), *Object->GetName());
            return false;
        }
    }

    FString ModuleName;
    UObject* CDO = Class->GetDefaultObject();
    CDO->ProcessEvent(Func, &ModuleName);
    if (ModuleName.IsEmpty())
        return false;

#if !UE_BUILD_SHIPPING
    if (GLuaDynamicBinding.IsValid(Class) && GLuaDynamicBinding.ModuleName != ModuleName)
    {
        UE_LOG(LogUnLua, Warning, TEXT("Dynamic binding '%s' ignored as it conflicts static binding '%s'."), *GLuaDynamicBinding.ModuleName, *ModuleName);
    }
#endif

    return Manager->Bind(Object, Class, *ModuleName, GLuaDynamicBinding.InitializerTableRef);
}

void FLuaContext::AddSearcher(int (*Searcher)(lua_State *), int Index)
{
    // if #package.searchers 
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "searchers");
    lua_remove(L, -2);
    if(!lua_istable(L, -1))
    {
        UE_LOG(LogUnLua, Warning, TEXT("Invalid package.serachers!"));
        return;
    }

    const uint32 Len = lua_rawlen(L, -1);
    Index = Index < 0 ? (int)(Len + Index + 2) : Index;
    for (int e = (int)Len + 1; e > Index; e--)
    {
        lua_rawgeti(L, -1, e - 1);
        lua_rawseti(L, -2, e);
    }
    
    lua_pushcfunction(L, Searcher);
    lua_rawseti(L, -2, Index);
    lua_pop(L, 1);
}

/**
 * Callback for FWorldDelegates::OnWorldTickStart
 */
#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION > 23)
void FLuaContext::OnWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaTime)
#else
void FLuaContext::OnWorldTickStart(ELevelTick TickType, float DeltaTime)
#endif
{
    if (!Manager)
    {
        return;
    }

    for (UInputComponent* InputComponent : CandidateInputComponents)
    {
        if (!InputComponent->IsRegistered() || InputComponent->IsPendingKill())
        {
            continue;
        }

        AActor* Actor = Cast<AActor>(InputComponent->GetOuter());
        Manager->ReplaceInputs(Actor, InputComponent);                              // try to replace/override input events
    }

    CandidateInputComponents.Empty();
    FWorldDelegates::OnWorldTickStart.Remove(OnWorldTickStartHandle);
}

/**
 * Callback for FWorldDelegates::OnWorldCleanup
 */
void FLuaContext::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
    if (!World || !bEnable)
    {
        return;
    }

    World->RemoveOnActorSpawnedHandler(OnActorSpawnedHandle);

#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION > 23)
    Cleanup(IsEngineExitRequested(), World);                    // clean up
#else
    Cleanup(GIsRequestingExit, World);                          // clean up
#endif
}

/**
 * Callback for FCoreDelegates::OnPostEngineInit
 */
void FLuaContext::OnPostEngineInit()
{
#if AUTO_UNLUA_STARTUP && !WITH_EDITOR
    SetEnable(true);
#endif

    CreateDefaultParamCollection();                 // create data for default parameters of UFunctions

#if WITH_EDITOR
    UGameViewportClient* GameViewportClient = GEngine->GameViewport;
    if (GameViewportClient)
    {
        GameViewportClient->OnGameViewportInputKey().BindRaw(this, &FLuaContext::OnGameViewportInputKey);   // bind a default input event
    }
#endif
}

/**
 * Callback for FCoreDelegates::OnPreExit
 */
void FLuaContext::OnPreExit()
{
    Cleanup(true);                                  // full clean up
}

/**
 * Callback for FCoreDelegates::OnAsyncLoadingFlushUpdate
 */
void FLuaContext::OnAsyncLoadingFlushUpdate()
{
    if (!Manager)
        return;

    TArray<UObject*> LocalCandidates;
    {
        FScopeLock Lock(&Async2MainCS);

        static constexpr EInternalObjectFlags AsyncObjectFlags = EInternalObjectFlags::AsyncLoading | EInternalObjectFlags::Async;
        for (int32 i = Candidates.Num() - 1; i >= 0; --i)
        {
            FWeakObjectPtr ObjectPtr = Candidates[i];
            if (!ObjectPtr.IsValid())
            {
                // discard invalid objects
                Candidates.RemoveAt(i);
                continue;
            }

            UObject* Object = ObjectPtr.Get();
            if (Object->HasAnyFlags(RF_NeedPostLoad)
                || Object->HasAnyInternalFlags(AsyncObjectFlags)
                || Object->GetClass()->HasAnyInternalFlags(AsyncObjectFlags))
            {
                // delay bind on next update 
                continue;
            }
            
            LocalCandidates.Add(Object);
            Candidates.RemoveAt(i);
        }
    }

    for (int32 i = 0; i < LocalCandidates.Num(); ++i)
    {
        UObject* Object = LocalCandidates[i];
        TryToBindLua(Object);
    }
}

/**
 * Callback for FCoreDelegates::OnHandleSystemError and FCoreDelegates::OnHandleSystemEnsure
 */
void FLuaContext::OnCrash()
{
    if (!IsInGameThread())
        return;

    const FString LogStr = UnLua::GetLuaCallStack(L);

    if (LogStr.IsEmpty())
    {
        UE_LOG(LogUnLua, Warning, TEXT("Lua state has not been created yet."));
    }
    else
    {
        UE_LOG(LogUnLua, Error, TEXT("%s"), *LogStr);
    }

    GLog->Flush();
}


/**
 * Callback for FCoreUObjectDelegates::PostLoadMapWithWorld
 */
void FLuaContext::PostLoadMapWithWorld(UWorld* World)
{
    if (!World || !bEnable)
    {
        return;
    }

#if !WITH_EDITOR

    // !!!Fix!!!
    // gameinstance delay bind, muti lua state support
    UGameInstance* GameInstance = World->GetGameInstance();
    if (GameInstance
        && (!GameInstances.Contains(GameInstance)))
    {
        TryToBindLua(GameInstance);                     // try to bind Lua module for GameInstance
        GameInstances.Add(GameInstance);

        // try to bind Lua module for UGameInstanceSubsystem
        auto Subsystems= GameInstance->GetSubsystemArray<UGameInstanceSubsystem>();
        for (auto Subsystem: Subsystems)
        { 
            TryToBindLua(Subsystem);
        }
    }

#endif

    Manager->OnMapLoaded(World);

    // !!!Fix!!!
    // when world is cleanup, this need to remove
    // register callback for spawning an actor
    OnActorSpawnedHandle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateUObject(Manager, &UUnLuaManager::OnActorSpawned));
}

#if WITH_EDITOR
/**
 * Callback for FEditorDelegates::PreBeginPIE
 */
void FLuaContext::PreBeginPIE(bool bIsSimulating)
{
#if AUTO_UNLUA_STARTUP
    SetEnable(true);
#endif

    UGameViewportClient* GameViewportClient = GEngine->GameViewport;
    if (GameViewportClient)
    {
        GameViewportClient->OnGameViewportInputKey().BindRaw(this, &FLuaContext::OnGameViewportInputKey);   // bind a default input event
    }
}

/**
 * Callback for FEditorDelegates::PostPIEStarted
 */
void FLuaContext::PostPIEStarted(bool bIsSimulating)
{
    UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEngine);
    if (EditorEngine)
    {
        PostLoadMapWithWorld(EditorEngine->PlayWorld);
    }
}

/**
 * Callback for FEditorDelegates::PrePIEEnded
 */
void FLuaContext::PrePIEEnded(bool bIsSimulating)
{
    // close lua env alwaylls
    SetEnable(false);
}

#endif

/**
 * Add a Lua coroutine and its reference in Lua registry
 */
void FLuaContext::AddThread(lua_State* Thread, int32 ThreadRef)
{
    ThreadToRef.Add(Thread, ThreadRef);
    RefToThread.Add(ThreadRef, Thread);
}

/**
 * Starts and resumes a Lua coroutine
 */
void FLuaContext::ResumeThread(int32 ThreadRef)
{
    lua_State** ThreadPtr = RefToThread.Find(ThreadRef);
    if (ThreadPtr)
    {
        lua_State* Thread = *ThreadPtr;
#if 504 == LUA_VERSION_NUM
        int NResults = 0;
        int32 State = lua_resume(Thread, L, 0, &NResults);
#else
        int32 State = lua_resume(Thread, L, 0);
#endif
        if (State == LUA_OK)
        {
            ThreadToRef.Remove(Thread);
            RefToThread.Remove(ThreadRef);
            luaL_unref(L, LUA_REGISTRYINDEX, ThreadRef);    // remove the reference if the coroutine finishes its execution
        }
    }
}

/**
 * Clean up all Lua coroutines
 */
void FLuaContext::CleanupThreads()
{
    ThreadToRef.Empty();
    RefToThread.Empty();
}

/**
 * Find a Lua coroutine
 */
int32 FLuaContext::FindThread(lua_State* Thread)
{
    int32* ThreadRefPtr = ThreadToRef.Find(Thread);
    return ThreadRefPtr ? *ThreadRefPtr : LUA_REFNIL;
}

/**
 * Callback when a UObjectBase (not full UObject) is created
 */
void FLuaContext::NotifyUObjectCreated(const UObjectBase* InObject, int32 Index)
{
    {
        FScopeLock Lock(&Async2MainCS);
        UObjPtr2Idx.Add(const_cast<UObjectBase*>(InObject), Index);
#if UNLUA_ENABLE_DEBUG != 0
        UObjPtr2Name.Add(const_cast<UObjectBase*>(InObject), InObject->GetFName().ToString());
#endif
    }

    if (!bEnable)
    {
        return;
    }

#if WITH_EDITOR
    // Don't bind during cook
    if (GIsCookerLoadingPackage)
    {
        return;
    }
#endif

    // try to bind a Lua module for the object
    UObject* Object = (UObject*)InObject;
    TryToBindLua(Object);

    // special handling for UInputComponent
    if (!Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) && Object->IsA<UInputComponent>())
    {
        AActor* Actor = Cast<APlayerController>(Object->GetOuter());
        if (!Actor)
        {
            Actor = Cast<APawn>(Object->GetOuter());
        }
        if (Actor && Actor->GetLocalRole() >= ROLE_AutonomousProxy)
        {
            //!!!Fix!!!
            // when tick start processing, inputcomponent may be invald or changeing
            CandidateInputComponents.AddUnique((UInputComponent*)InObject);
            if (OnWorldTickStartHandle.IsValid())
            {
                FWorldDelegates::OnWorldTickStart.Remove(OnWorldTickStartHandle);
            }
            OnWorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddRaw(this, &FLuaContext::OnWorldTickStart);
        }
    }
}

/**
 * Callback when a UObjectBase (not full UObject) is deleted
 */
void FLuaContext::NotifyUObjectDeleted(const UObjectBase* InObject, int32 Index)
{
    if (!bEnable)
    {
        FScopeLock Lock(&Async2MainCS);
        UObjPtr2Idx.Remove(InObject);

#if UNLUA_ENABLE_DEBUG != 0
        UObjPtr2Name.Remove(InObject);
#endif

        return;
    }

#if UNLUA_ENABLE_DEBUG != 0
    UE_LOG(LogUnLua, Log, TEXT("NotifyUObjectDeleted : %s,%p"), *UObjPtr2Name[InObject], InObject);
#endif

    bool bClass = GReflectionRegistry.NotifyUObjectDeleted(InObject);
    Manager->NotifyUObjectDeleted(InObject, bClass);
    FDelegateHelper::NotifyUObjectDeleted((UObject*)InObject);

    if (CandidateInputComponents.Num() > 0)
    {
        int32 NumRemoved = CandidateInputComponents.Remove((UInputComponent*)InObject);
        if (NumRemoved > 0 && CandidateInputComponents.Num() < 1)
        {
            FWorldDelegates::OnWorldTickStart.Remove(OnWorldTickStartHandle);
        }
    }

    FScopeLock Lock(&Async2MainCS);
    UObjPtr2Idx.Remove(InObject);
    UObjPtr2Name.Remove(InObject);
}


/**
 * Callback when a GUObjectArray is deleted
 */
#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION > 22)
void FLuaContext::OnUObjectArrayShutdown()
{
#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION > 23)
    bool bEngineExit = IsEngineExitRequested();
#else
    bool bEngineExit = GIsRequestingExit;
#endif

    if (bEngineExit)
    {
        // when exiting, remove listeners for creating/deleting UObject
        GUObjectArray.RemoveUObjectCreateListener(GLuaCxt);
        GUObjectArray.RemoveUObjectDeleteListener(GLuaCxt);
    }
}
#endif

/**
 * Robust method to verify uobject
 */
bool FLuaContext::IsUObjectValid(UObjectBase* UObjPtr)
{
    if (!UObjPtr)
    {
        return false;
    }

    int32 UObjIdx = -1;
    {
        FScopeLock Lock(&Async2MainCS);
        if (UObjPtr2Idx.Contains(UObjPtr))
        {
            UObjIdx = UObjPtr2Idx[UObjPtr];
        }
    }

    if (-1 != UObjIdx)
    {
        FUObjectItem* UObjectItem = GUObjectArray.IndexToObject(UObjIdx);
        if (!UObjectItem)
        {
            return false;
        }
        else
        {
            return (UObjPtr == UObjectItem->Object) && ((UObjPtr->GetFlags() & (RF_BeginDestroyed | RF_FinishDestroyed)) == 0)
                    && !UObjectItem->IsUnreachable();
        }
    }
    else
    {
        //!!!Fix!!!
        //all should be false here?
        return false;
    }
}

UUnLuaManager* FLuaContext::GetUnLuaManager()
{
    return Manager;
}


FLuaContext::FLuaContext()
    : L(nullptr), Manager(nullptr), bEnable(false)
{
#if WITH_EDITOR
    LuaHandle = nullptr;
#endif
}

FLuaContext::~FLuaContext()
{
    Cleanup(true);

    if (Manager)
    {
        Manager->RemoveFromRoot();
        Manager = nullptr;
    }

    if (L)
    {
        L = NULL;
    }

#if ENGINE_MAJOR_VERSION <= 4 && ENGINE_MINOR_VERSION < 23
    // when exiting, remove listeners for creating/deleting UObject
    GUObjectArray.RemoveUObjectCreateListener(GLuaCxt);
    GUObjectArray.RemoveUObjectDeleteListener(GLuaCxt);
#endif

    FScopeLock Lock(&Async2MainCS);
    UObjPtr2Idx.Empty();

#if UNLUA_ENABLE_DEBUG != 0
    UObjPtr2Name.Empty();
#endif
}

/**
 * Allocator for Lua VM
 */
void* FLuaContext::LuaAllocator(void* ud, void* ptr, size_t osize, size_t nsize)
{
    if (nsize == 0)
    {
#if STATS
        const uint32 Size = FMemory::GetAllocSize(ptr);
        DEC_MEMORY_STAT_BY(STAT_UnLua_Lua_Memory, Size);
#endif
        FMemory::Free(ptr);
        return nullptr;
    }

    void* Buffer;
    if (!ptr)
    {
        Buffer = FMemory::Malloc(nsize);
#if STATS
        const uint32 Size = FMemory::GetAllocSize(Buffer);
        INC_MEMORY_STAT_BY(STAT_UnLua_Lua_Memory, Size);
#endif
    }
    else
    {
#if STATS
        const uint32 OldSize = FMemory::GetAllocSize(ptr);
#endif
        Buffer = FMemory::Realloc(ptr, nsize);
#if STATS
        const uint32 NewSize = FMemory::GetAllocSize(Buffer);
        if (NewSize > OldSize)
        {
            INC_MEMORY_STAT_BY(STAT_UnLua_Lua_Memory, NewSize - OldSize);
        }
        else
        {
            DEC_MEMORY_STAT_BY(STAT_UnLua_Lua_Memory, OldSize - NewSize);
        }
#endif
    }
    return Buffer;
}

/**
 * Initialize UnLua
 */
void FLuaContext::Initialize()
{
    if (!bEnable)
    {
        CreateState();  // create Lua main thread

        // create UnLuaManager and add it to root
        Manager = NewObject<UUnLuaManager>();
        Manager->AddToRoot();

        if (L)
        {
            GPropertyCreator.Cleanup();
            bEnable = true;
            FUnLuaDelegates::OnLuaContextInitialized.Broadcast();
        }
    }
}

/**
 * Clean up UnLua
 */
void FLuaContext::Cleanup(bool bFullCleanup, UWorld* World)
{
    if (!bEnable)
    {
        return;
    }

    if (L)
    {
        FUnLuaDelegates::OnPreLuaContextCleanup.Broadcast(bFullCleanup);

        if (!bFullCleanup)
        {
            // force full lua gc
            lua_gc(L, LUA_GCCOLLECT, 0);
            lua_gc(L, LUA_GCCOLLECT, 0);

            //!!!Fix!!!
            // do some check work here
        }
        else
        {
            bEnable = false;

            // close lua state first
            lua_close(L);
            L = nullptr;

            // clean ue side modules,es static data structs
            FCollisionHelper::Cleanup();                        // clean up collision helper stuff

            GObjectReferencer.Cleanup();                        // clean up object referencer

            //!!!Fix!!!
            //thread need refine
            CleanupThreads();                                   // lua thread

            LibraryNames.Empty();                               // metatables and lua module
            ModuleNames.Empty();

            FDelegateHelper::Cleanup(bFullCleanup);                 // clean up delegates

            Manager->Cleanup(NULL, bFullCleanup);                  // clean up UnLuaManager

            GPropertyCreator.Cleanup();                             // clean up dynamically created UProperties

            GReflectionRegistry.Cleanup();                      // clean up reflection registry

            GameInstances.Empty();
            CandidateInputComponents.Empty();
            FCoreUObjectDelegates::GetPostGarbageCollect().Remove(OnPostGarbageCollectHandle);
            FWorldDelegates::OnWorldTickStart.Remove(OnWorldTickStartHandle);

            // old manager
            if (Manager)
            {
                Manager->RemoveFromRoot();
                Manager = nullptr;
            }

#if WITH_EDITOR
            if (LuaHandle)
            {
                FPlatformProcess::FreeDllHandle(LuaHandle);     // unload Lua dynamic lib
                LuaHandle = nullptr;
            }
#endif

        }

        FUnLuaDelegates::OnPostLuaContextCleanup.Broadcast(bFullCleanup);
    }
}

/**
 * Build-in input event for 'Hotfix'
 */
bool FLuaContext::OnGameViewportInputKey(FKey InKey, FModifierKeysState ModifierKeyState, EInputEvent EventType)
{
    if (!bEnable)
    {
        return false;
    }
    if (InKey == EKeys::L && ModifierKeyState.IsControlDown() && EventType == IE_Released)
    {
        return HotfixLua();
    }
    return false;
}
/**
 * ============================================================
 * mono_api.h — Mono Embedding API 不透明类型声明
 * ============================================================
 * 第一阶段通过 GetProcAddress 动态解析 Mono 导出，不链接 mono.lib，
 * 因此这里只声明指针类型需要的不透明结构，不复制 Mono 内部布局。
 *
 * 禁止在其他模块中猜测这些结构的字段。所有操作必须经过导出函数，
 * 以减少 Unity 所携带 Mono 版本差异造成的崩溃风险。
 * ============================================================
 */
#pragma once

#include <cstdint>

// Mono 元数据和对象均由目标进程中的运行时拥有。
struct _MonoDomain;     using MonoDomain = _MonoDomain;
struct _MonoThread;     using MonoThread = _MonoThread;
struct _MonoAssembly;   using MonoAssembly = _MonoAssembly;
struct _MonoImage;      using MonoImage = _MonoImage;
struct _MonoClass;      using MonoClass = _MonoClass;
struct _MonoMethod;     using MonoMethod = _MonoMethod;
struct _MonoClassField; using MonoClassField = _MonoClassField;
struct _MonoType;       using MonoType = _MonoType;
struct _MonoObject;     using MonoObject = _MonoObject;
struct _MonoString;     using MonoString = _MonoString;
struct _MonoArray;      using MonoArray = _MonoArray;
struct _MonoVTable;     using MonoVTable = _MonoVTable;
struct _MonoMethodSignature; using MonoMethodSignature = _MonoMethodSignature;
struct _MonoTableInfo; using MonoTableInfo = _MonoTableInfo;

using MonoAssemblyForeachCallback = void(*)(MonoAssembly*, void*);

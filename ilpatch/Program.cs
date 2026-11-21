// ilpatch: small, declarative method-body replacements for the game's managed assemblies (Mono 2.x era).
//   ilpatch <in.dll> <out.dll> <recipe> [<recipe> ...]
// recipes (Type::Method/argc selects an overload by parameter count):
//   (Type::Method/n or Type::Method/ParamType,ParamType selects an overload)
//   stub:Type::Method/n              body returns the default value (null / 0 / false / nothing)
//   return-new:Type::Method/n        body returns `new ReturnType()`
//   return-int:Type::Method/n=V      body returns the int constant V (also bool: 0/1)
//   readexact:Type::Method/n@s,c     body returns a byte[] of length arg c read fully from Stream arg s
//                                    (used to replace slow managed decryption by pre-decrypted data)
using System;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

static class P {
    static MethodDefinition Find(ModuleDefinition m, string spec) {
        // overload selector after '/': a parameter count ("/2") or parameter type names ("/ZipFile,Int32")
        string sel = spec; int argc = -1; string[] ptypes = null;
        int slash = sel.LastIndexOf('/');
        if (slash > 0) {
            string o = sel[(slash + 1)..]; sel = sel[..slash];
            if (!int.TryParse(o, out argc)) { argc = -1; ptypes = o.Split(','); }
        }
        int k = sel.IndexOf("::"); string tn = sel[..k], mn = sel[(k + 2)..];
        var t = m.GetTypes().FirstOrDefault(x => x.FullName == tn) ?? throw new Exception("type not found: " + tn);
        var c = t.Methods.Where(x => x.Name == mn && (argc < 0 || x.Parameters.Count == argc) &&
                                     (ptypes == null || (x.Parameters.Count == ptypes.Length &&
                                      x.Parameters.Select((p, i) => p.ParameterType.Name == ptypes[i]).All(b => b)))).ToList();
        if (c.Count != 1) throw new Exception($"{spec}: {c.Count} matches");
        return c[0];
    }
    static ILProcessor Reset(MethodDefinition md) {
        md.Body = new MethodBody(md); md.Body.InitLocals = true;
        return md.Body.GetILProcessor();
    }
    static void Default(ILProcessor il, MethodDefinition md) {
        var rt = md.ReturnType;
        if (rt.MetadataType == MetadataType.Void) { }
        else if (rt.IsValueType && rt.MetadataType != MetadataType.Int64 && rt.MetadataType != MetadataType.UInt64 &&
                 rt.MetadataType != MetadataType.Single && rt.MetadataType != MetadataType.Double && rt.IsPrimitive) il.Emit(OpCodes.Ldc_I4_0);
        else if (rt.MetadataType == MetadataType.Int64 || rt.MetadataType == MetadataType.UInt64) { il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Conv_I8); }
        else if (rt.MetadataType == MetadataType.Single) il.Emit(OpCodes.Ldc_R4, 0f);
        else if (rt.MetadataType == MetadataType.Double) il.Emit(OpCodes.Ldc_R8, 0d);
        else if (rt.IsValueType) { var v = new VariableDefinition(rt); md.Body.Variables.Add(v); il.Emit(OpCodes.Ldloca, v); il.Emit(OpCodes.Initobj, rt); il.Emit(OpCodes.Ldloc, v); }
        else il.Emit(OpCodes.Ldnull);
        il.Emit(OpCodes.Ret);
    }
    static int Main(string[] a) {
        if (a.Length < 3) { Console.Error.WriteLine("usage: ilpatch in.dll out.dll recipe..."); return 2; }
        var rp = new DefaultAssemblyResolver(); rp.AddSearchDirectory(System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(a[0])));
        var asm = AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters { AssemblyResolver = rp, ReadingMode = ReadingMode.Immediate });
        var m = asm.MainModule;
        foreach (var r in a.Skip(2)) {
            int colon = r.IndexOf(':'); string kind = r[..colon], rest = r[(colon + 1)..];
            string extra = null;
            int at = rest.IndexOfAny(new[] { '@', '=' }); if (at > 0) { extra = rest[(at + 1)..]; rest = rest[..at]; }
            var md = Find(m, rest);
            var il = Reset(md);
            switch (kind) {
            case "stub": Default(il, md); break;
            case "return-int": il.Emit(OpCodes.Ldc_I4, int.Parse(extra)); il.Emit(OpCodes.Ret); break;
            case "return-new": {
                var ctor = md.ReturnType.Resolve().Methods.First(x => x.IsConstructor && !x.IsStatic && x.Parameters.Count == 0);
                il.Emit(OpCodes.Newobj, m.ImportReference(ctor)); il.Emit(OpCodes.Ret); break; }
            case "readexact": {
                var sp = extra.Split(','); int s = int.Parse(sp[0]), cnt = int.Parse(sp[1]);
                var pS = md.Parameters[s]; var pC = md.Parameters[cnt];
                var bytes = new VariableDefinition(m.ImportReference(typeof(byte[])));
                var off = new VariableDefinition(m.TypeSystem.Int32); var n = new VariableDefinition(m.TypeSystem.Int32);
                md.Body.Variables.Add(bytes); md.Body.Variables.Add(off); md.Body.Variables.Add(n);
                var read = m.ImportReference(typeof(System.IO.Stream).GetMethod("Read", new[] { typeof(byte[]), typeof(int), typeof(int) }));
                // stream type in the game references the game's mscorlib, rewrite scope to it
                read = m.ImportReference(pS.ParameterType.Resolve().Methods.First(x => x.Name == "Read" && x.Parameters.Count == 3));
                var loop = il.Create(OpCodes.Nop); var done = il.Create(OpCodes.Ldloc, bytes);
                il.Emit(OpCodes.Ldarg, pC); il.Emit(OpCodes.Newarr, m.TypeSystem.Byte); il.Emit(OpCodes.Stloc, bytes);
                il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Stloc, off);
                il.Append(loop);
                il.Emit(OpCodes.Ldloc, off); il.Emit(OpCodes.Ldarg, pC); il.Emit(OpCodes.Bge, done);
                il.Emit(OpCodes.Ldarg, pS); il.Emit(OpCodes.Ldloc, bytes); il.Emit(OpCodes.Ldloc, off);
                il.Emit(OpCodes.Ldarg, pC); il.Emit(OpCodes.Ldloc, off); il.Emit(OpCodes.Sub);
                il.Emit(OpCodes.Callvirt, read); il.Emit(OpCodes.Stloc, n);
                il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ble, done);
                il.Emit(OpCodes.Ldloc, off); il.Emit(OpCodes.Ldloc, n); il.Emit(OpCodes.Add); il.Emit(OpCodes.Stloc, off);
                il.Emit(OpCodes.Br, loop);
                il.Append(done); il.Emit(OpCodes.Ret);
                break; }
            default: throw new Exception("unknown recipe " + kind);
            }
            Console.WriteLine($"   ilpatch {kind}: {md.FullName}");
        }
        asm.Write(a[1]);
        return 0;
    }
}

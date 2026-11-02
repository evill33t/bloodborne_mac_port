// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

// A bounded source-goal adapter. The source helpers live inside the same
// owned goal chunk, so no global game helpers are overwritten and no separate
// custom helper file depends on map-bank load order.
internal static class SourceEnemyAi {
    internal const int FoliageGoal=882500;
    internal const string FoliageName="DS1_Foliage_Jujin233000Battle";
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Require(bool b,string why){if(!b)throw new InvalidDataException("Source enemy AI: "+why);}
    internal static byte[] Foliage(byte[] destination,Func<string,byte[]?> readSource,Func<string,byte[]> readNative) {
        var source=BND3.Read(readSource("script/m12_00_00_00.luabnd.dcx")??throw new FileNotFoundException("Darkroot source AI unavailable"));
        var common=BND3.Read(readSource("script/aicommon.luabnd.dcx")??throw new FileNotFoundException("Source common AI unavailable"));
        var nativeCommon=BND4.Read(readNative("script/aicommon.luabnd.dcx"));
        var entry=source.Files.Single(f=>Leaf(f.Name)=="233000_battle.lua");
        // These source helpers contain two unreachable legacy nil branches.
        // Pin the audited programs: foliage never assigns positive weights to
        // defAct10/11 and always supplies its explicit after-action callback.
        void Audited(BinderFile f,string hash)=>Require(Convert.ToHexStringLower(SHA256.HashData(f.Bytes.Span))==hash,"source AI program changed: "+Leaf(f.Name));
        Audited(entry,"4f2dd9d5e7b84394266f2c391736d0c444e42b54aa497974ca70019b3ff7a035");
        Audited(common.Files.Single(f=>Leaf(f.Name)=="common_battle_func.lua"),"8f8fa6f4617fe165e3aba2f5c22234beae7b3c3907162b3fb608f0ed3faa1e89");
        Audited(common.Files.Single(f=>Leaf(f.Name)=="common_func2.lua"),"52e089b88fbb6e22c04174cc362052b3b82ccaddea91078e3727da285fb1cb08");
        var root=Lua50.Read(entry.Bytes.ToArray());
        var exports=new Dictionary<string,Lua50.Export>(StringComparer.Ordinal);
        foreach(string name in new[]{"common_battle_func.lua","common_func2.lua"})foreach(var (key,value) in Lua50.Exports(Lua50.Read(common.Files.Single(f=>Leaf(f.Name)==name).Bytes.ToArray())))
            Require(exports.TryAdd(key,value),"ambiguous source helper: "+key);
        var selected=new Dictionary<string,Lua50.Export>(StringComparer.Ordinal);
        void Dependencies(Lua50.Function f) {
            foreach(string name in Lua50.Globals(f))if(exports.TryGetValue(name,out var helper)&&selected.TryAdd(name,helper))Dependencies(helper.Function);
        }
        Dependencies(root);Require(selected.Count==37,"foliage helper closure changed");
        foreach(var (name,helper) in selected)Require(helper.Captures.Count==0||
            (name is "Common_Clear_Param" or "Common_Battle_Activate")&&helper.Captures.SequenceEqual(new[]{20d}),"unverified source helper capture");
        // Source3004 is a paired grab. Until its attacker/victim pair is
        // converted, redirect that decision and interrupt to the left claw.
        root=Lua50.Read(Lua50.Rewrite(Lua50.Write(root),new Dictionary<string,string>(),new Dictionary<string,double>(),new Dictionary<double,double>{{3004,3003}}));
        Lua50.PrependFunctions(root,selected);
        Dictionary<string,double> Definitions(IEnumerable<BinderFile> files) {
            var values=new Dictionary<string,double>(StringComparer.Ordinal);
            foreach(string name in new[]{"ai_define.lua","goal_list.lua"})foreach(var (key,value) in Lua50.Literals(Lua50.Read(files.Single(f=>Leaf(f.Name)==name).Bytes.ToArray())))
                Require(values.TryAdd(key,value)||values[key]==value,"ambiguous engine enum");
            return values;
        }
        var sourceEnums=Definitions(common.Files);var nativeEnums=Definitions(nativeCommon.Files);
        Require(sourceEnums.GetValueOrDefault("GOAL_Jujin233000_Battle")==233000,"source goal definition changed");
        var owned=Lua50.Globals(root,true).Concat(selected.Keys).Append("Jujin233000Battle")
            .Concat(new[]{"Tbl","atkAfterOddsTbl"}).ToHashSet(StringComparer.Ordinal);
        foreach(string name in Lua50.Globals(root)) {
            if(owned.Contains(name)||name is "REGISTER_GOAL" or "REGISTER_GOAL_NO_UPDATE" or "GOAL_Jujin233000_Battle")continue;
            Require(sourceEnums.TryGetValue(name,out double value)&&nativeEnums.TryGetValue(name,out double native)&&value==native,"unsupported source enum/API global: "+name);
        }
        // Keep the unreachable legacy nil branches private as well; unrelated
        // native goals must not supply values to these source-only globals.
        var strings=Lua50.All(root).SelectMany(f=>f.Constants).OfType<byte[]>().Select(System.Text.Encoding.UTF8.GetString).ToHashSet(StringComparer.Ordinal);
        var names=owned.Where(strings.Contains).ToDictionary(n=>n,n=>"DS1_Foliage_"+n,StringComparer.Ordinal);
        var bytes=Lua50.Rewrite(Lua50.Write(root),names,new Dictionary<string,double>{{"GOAL_Jujin233000_Battle",FoliageGoal}});
        var target=BND4.Read(destination);var infoFile=target.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal));var info=LUAINFO.Read(infoFile.Bytes);
        Require(!info.Goals.Any(g=>g.ID==FoliageGoal||g.Name==FoliageName),"owned foliage goal is occupied");
        string leaf=FoliageGoal+"_battle.lua";Require(!target.Files.Any(f=>Leaf(f.Name)==leaf),"owned foliage script is occupied");
        int id=1000;var ids=target.Files.Select(f=>f.ID).ToHashSet();while(ids.Contains(id))id++;Require(id<1000000,"AI binder IDs exhausted");
        target.Files.Add(new BinderFile(entry.Flags,id,@"N:\SPRJ\data\INTERROOT_ps4\script\ai\out\"+leaf,bytes));
        info.Goals.Add(new LUAINFO.Goal(FoliageGoal,FoliageName,true,false,null));infoFile.Bytes=info.Write();
        var namesFile=target.Files.Single(f=>Leaf(f.Name).EndsWith(".luagnl",StringComparison.Ordinal));var globals=LUAGNL.Read(namesFile.Bytes);var existing=globals.Globals.ToHashSet(StringComparer.Ordinal);
        foreach(string name in names.Values.Order(StringComparer.Ordinal))if(existing.Add(name))globals.Globals.Add(name);
        namesFile.Bytes=globals.Write();target.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));return target.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}

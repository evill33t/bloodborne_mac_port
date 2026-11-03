// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
static class HydraActionsTests {
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string s){if(!b)throw new Exception(s);}
    static void Reject(Action a){try{a();}catch(Exception e)when(e is InvalidDataException or FileNotFoundException){return;}throw new Exception("Invalid action/AI accepted");}
    static void Main(string[] args) {
        var inputs=new Dictionary<string,byte[]>();
        byte[] Read(string path){var b=File.ReadAllBytes(path);inputs[path]=SHA256.HashData(b);return b;}
        byte[]? Source(string p)=>File.Exists(Path.Combine(args[0],p))?Read(Path.Combine(args[0],p)):null;
        byte[] Native(string p)=>Read(Path.Combine(args[1],p));
        var actions=new SourceCharacterActions(Source,"c3530");
        var near=actions.Resolve(3000);var split=actions.Resolve(3005);var alias=actions.Resolve(3008);var physical=actions.Resolve(3004);
        Need(near.Clip.Bones.Count==246&&split.Clip.Bones.Count==246,"Source rig population changed");
        Need(split.Archive=="chr/c3530_c3530.anibnd.dcx"&&split.MotionID==3005,"Declared source split not resolved");
        Need(alias.EventID==3008&&alias.MotionID==3004&&ReferenceEquals(alias.Clip,physical.Clip),"HKX-only alias lost identity or motion sharing");
        Need(!alias.Events.Select(e=>Convert.ToHexString(e.GetParameterBytes(false))).SequenceEqual(physical.Events.Select(e=>Convert.ToHexString(e.GetParameterBytes(false)))),"HKX alias inherited wrong projectile events");
        foreach(int id in new[]{0,8000,9000,9060})Need(actions.Resolve(id).Clip.Duration>0,"Valid idle/recoil/split failed");
        var original=BND3.Read(Source("chr/c3530.anibnd.dcx")!);var eventFile=original.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));var tae=TAE.Read(eventFile.Bytes);
        tae.Animations.Single(a=>a.ID==3008).MiniHeader=new TAE.Animation.AnimMiniHeader.ImportOtherAnim {ImportFromAnimID=3008};eventFile.Bytes=tae.Write();var cycle=original.Write(DCX.Type.DCX_DFLT_10000_44_9);
        Reject(()=>new SourceCharacterActions(p=>p=="chr/c3530.anibnd.dcx"?cycle:Source(p),"c3530").Resolve(3008));
        Reject(()=>new SourceCharacterActions(p=>p=="chr/c3530_c3530.anibnd.dcx"?null:Source(p),"c3530").Resolve(3005));
        Reject(()=>new SourceCharacterActions(Source,"../c3530"));Reject(()=>actions.Resolve(9999));
        var ai=HydraAi.Build(Native("script/m24_00_00_00.luabnd.dcx"),Source,Native);var bank=BND4.Read(ai);
        var info=LUAINFO.Read(bank.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)).Bytes);
        Need(info.Goals.Single(g=>g.ID==HydraAi.Goal).Name==HydraAi.Name,"Owned goal registration wrong");
        var root=Lua50.Read(bank.Files.Single(f=>Leaf(f.Name)=="883000_battle.lua").Bytes.ToArray());
        Need(Lua50.Globals(root,true).All(n=>n.StartsWith("DS1_Hydra_",StringComparison.Ordinal)||n=="OnIf_883000"),"Hydra overrides shared native globals");
        Need(Lua50.All(root).Any(f=>f.Constants.Contains(883000d))&&!Lua50.Globals(root).Contains("GOAL_LakeBeast_m12_353001_Battle"),"Owned goal constant missing");
        var sourceBank=BND3.Read(Source("script/m12_00_00_00.luabnd.dcx")!);var before=Lua50.Read(sourceBank.Files.Single(f=>Leaf(f.Name)=="353001_battle.lua").Bytes.ToArray());
        foreach(var (a,b) in Lua50.All(before).Zip(Lua50.All(root))) {
            Need(a.Code.Length==b.Code.Length&&a.Stack==b.Stack&&a.Upvalues==b.Upvalues,"AI control flow changed");
            foreach(var (x,y) in a.Code.Zip(b.Code))if(x!=y)Need((x&63)==5&&(y&63)==1&&(x&~63u)==(y&~63u),"AI branch or subgoal changed");
            Need(a.Constants.OfType<double>().SequenceEqual(b.Constants.OfType<double>().Where(d=>d!=883000)),"AI distance/attack constants changed");
        }
        Reject(()=>HydraAi.Build(ai,Source,Native));
        var fallback=MapNativeAi.Fallbacks(ai);Need(fallback.ContainsKey("script_interroot/ai/out/883000_battle.lua"),"Loose-path goal missing");
        var nativeSfx=Native("sfx/frpg_sfxbnd_m24.ffxbnd.dcx");var closure=HydraEffects.Merge(nativeSfx,Native(HydraEffects.NativePath));
        var outputSfx=BND4.Read(closure);foreach(int id in HydraEffects.Required)Need(outputSfx.Files.Any(f=>Leaf(f.Name)=="f"+id.ToString("D9")+".fxr"),"Fluid FXR missing");
        foreach(var f in BND4.Read(nativeSfx).Files)Need(outputSfx.Files.Single(g=>g.ID==f.ID).Bytes.Span.SequenceEqual(f.Bytes.Span),"Original destination SFX changed");
        Need(HydraEffects.Merge(closure,Native(HydraEffects.NativePath)).SequenceEqual(closure),"Fluid closure not idempotent");
        foreach(var (path,hash) in inputs)Need(SHA256.HashData(File.ReadAllBytes(path)).SequenceEqual(hash),"Input mutated "+path);
        Console.WriteLine($"PASS Hydra actions: source split, independent event/motion aliases, rejection, private source AI/unchanged decisions and {inputs.Count} unchanged archives");
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Text.RegularExpressions;

// Player-class NPCs always load c0000. Reserve private equipment banks rather
// than replacing the hunter's clips or relying on normalChangeAnimChrId.
internal static class HavelPlayerAnimations {
    internal const int WeaponBank=185,OneHandBank=85,BothHandsBank=86;
    internal sealed record Binding(int Bank,int Action,int Source,int Motion,float Duration,int Contacts);
    internal sealed record Result(byte[] Main,byte[] Split,IReadOnlyDictionary<string,byte[]> NativeSplits,IReadOnlyList<Binding> Bindings,int NativeCopies,int Selectors);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Havel player animation: "+why);}
    // Inspected native a105 selectors and DS1 a35 action/event imports. These
    // are semantic choices; DS1 3100/4100 are kicks, not heavy attacks.
    internal static readonly IReadOnlyDictionary<int,int> Attacks=new Dictionary<int,int> {
        [30000]=353000,[30100]=353001,[30200]=353002,[30300]=353000,[30400]=353001,
        [31000]=353300,[31100]=353301,[31200]=353310,[31010]=353300,[30010]=353000,
        [32100]=353500,[32150]=353500,[32300]=353600,[32500]=353900,[32550]=353900,
        [40000]=354000,[40100]=354001,[40200]=354002,[40300]=354000,
        [41000]=354300,[41100]=354301,[41200]=354310,[41300]=354301,[41010]=354300,[40010]=354000,
        [42100]=354500,[42150]=354500,[42300]=354600,[42500]=354900,[42550]=354900,
        [32620]=353300,[32630]=353300,[42620]=354300,[42630]=354300,[42660]=354300
    };
    static readonly IReadOnlyDictionary<int,int> Holds=new Dictionary<int,int> {
        [0]=0,[1]=1,[1000]=100,[1001]=101,
        [3000]=200,[3001]=201,[3002]=202,[3003]=203,
        [4000]=300,[4001]=301,[4002]=302,[4003]=303,[5000]=500
    };
    internal static Result Build(Func<string,byte[]?> source,Func<string,byte[]> native,IReadOnlyList<BoneMatch> bones) {
        var main=BND4.Read(native("chr/c0000.anibnd.dcx"));
        var graph=main.Files.Single(f=>f.ID==7000500);var character=main.Files.Single(f=>f.ID==7000400);
        var events=new Dictionary<int,TAE.Animation>();var taes=new Dictionary<int,TAE>();
        foreach(var file in main.Files.Where(f=>TAE.Is(f.Bytes.ToArray()))) {
            var m=Regex.Match(Leaf(file.Name),@"\Aa(\d{2,3})\.tae\z");Need(m.Success,"unrecognized native event bank");int bank=int.Parse(m.Groups[1].Value);
            var tae=TAE.Read(file.Bytes);Need(tae.ID==2000+bank&&!tae.BigEndian,"native event bank ABI changed");taes.Add(bank,tae);
            foreach(var a in tae.Animations)Need(a.ID is >=0 and <1000000&&events.TryAdd(checked(bank*1000000+(int)a.ID),a),"ambiguous native action");
        }
        var declarations=main.Files.Where(f=>f.ID is >=6000000 and <7000000&&Regex.IsMatch(Leaf(f.Name),@"\Ac0000_[A-Za-z0-9_]+\.txt\z")).ToArray();
        Need(declarations.Length is >0 and <=64,"invalid native split declarations");
        var motions=new Dictionary<int,BinderFile>();long budget=0;BND4? carrier=null;
        foreach(var declaration in declarations) {
            string path="chr/"+Leaf(declaration.Name)[..^4]+".anibnd.dcx";var bytes=native(path);
            Need(bytes.Length<=128*1024*1024&&(budget+=bytes.Length)<=512L*1024*1024,"native split read budget exceeded");
            var bank=BND4.Read(bytes);carrier??=bank;
            foreach(var file in bank.Files.Where(f=>f.ID>=1000000000))Need(motions.TryAdd(file.ID-1000000000,file),"ambiguous native physical clip");
        }
        int Physical(int id,HashSet<int>? seen=null) {
            seen??=[];Need(seen.Count<64&&seen.Add(id),"cyclic native action import");
            if(!events.TryGetValue(id,out var a))return motions.ContainsKey(id)?id:-1;
            return a.MiniHeader switch {
                TAE.Animation.AnimMiniHeader.ImportOtherAnim i=>Physical(i.ImportFromAnimID,seen),
                TAE.Animation.AnimMiniHeader.Standard s when s.ImportsHKX&&s.ImportHKXSourceAnimID>=0=>Physical(s.ImportHKXSourceAnimID,seen),
                TAE.Animation.AnimMiniHeader.Standard=>id,
                _=>throw new InvalidDataException("Unsupported native player action header")};
        }
        var bankMap=new Dictionary<int,int>{{105,WeaponBank},{0,OneHandBank},{22,BothHandsBank}};
        // Retail graph leaves also name removed authoring clips (e.g a000_2).
        // They have neither TAE rows nor HKXs. Do not invent private motions
        // for those dead leaves; keep the existing no-match fallback.
        IReadOnlySet<int> Available(int bank)=>events.Keys.Concat(motions.Keys).Where(id=>id/1000000==bank&&motions.ContainsKey(Physical(id))).Select(id=>id%1000000).ToHashSet();
        var appended=NativePlayerAnimationBindings.Append(graph.Bytes.ToArray(),character.Bytes.ToArray(),[
            new(105,WeaponBank,new HashSet<int>{13,14},Available(105)),new(0,OneHandBank,new HashSet<int>{11},Available(0)),new(22,BothHandsBank,new HashSet<int>{11},Available(22))]);
        var owned=new Dictionary<int,TAE>();
        foreach(var (from,to) in bankMap) {
            Need(!taes.ContainsKey(to)&&!main.Files.Any(f=>f.ID==5000000+to),"private event bank occupied");
            var clone=TAE.Read(taes[from].Write());clone.ID=2000+to;
            foreach(var a in clone.Animations) {
                a.AnimFileName=$"a{to:D3}_{a.ID:D6}.hkt";
                int Remap(int id)=>id/1000000==from?to*1000000+id%1000000:id;
                if(a.MiniHeader is TAE.Animation.AnimMiniHeader.ImportOtherAnim i)i.ImportFromAnimID=Remap(i.ImportFromAnimID);
                if(a.MiniHeader is TAE.Animation.AnimMiniHeader.Standard s&&s.ImportsHKX)s.ImportHKXSourceAnimID=Remap(s.ImportHKXSourceAnimID);
            }
            owned.Add(to,clone);
        }
        var sourceActions=new SourcePlayerActions(source);var skeleton=main.Files.Single(f=>f.ID==4000000).Bytes.ToArray();
        var generated=new Dictionary<int,byte[]>();var bindings=new List<Binding>();
        void Source(int bank,int action,int from) {
            var rows=owned[bank].Animations.Where(a=>a.ID==action).ToArray();Need(rows.Length==1,"mapped native action missing: "+bank+"/"+action);var row=rows[0];
            var clip=sourceActions.Resolve(from);Need(clip.Clip.FrameCount<=256,"source player motion exceeds single block");
            row.MiniHeader=new TAE.Animation.AnimMiniHeader.Standard {IsLoopByDefault=(row.MiniHeader as TAE.Animation.AnimMiniHeader.Standard)?.IsLoopByDefault??false};
            // Preserve native controller gates at normalized native timing,
            // replacing damage with source contacts in the source pose's time.
            int original=bankMap.Single(p=>p.Value==bank).Key*1000000+action;
            int physical=Physical(original);Need(motions.TryGetValue(physical,out var old),"native mapped motion missing");
            var pack=new NativeAnimation.Pack(old!.Bytes.ToArray());float oldDuration=pack.Float(pack.Object("hkaSplineCompressedAnimation")+16);Need(oldDuration>0,"invalid native motion duration");
            var keep=row.Events.Where(e=>e.Type is not(1 or 2 or 307)).Select(e=>{var copy=e.GetClone(false);copy.Group=e.Group;return copy;}).ToList();
            foreach(var e in keep){e.StartTime=Math.Clamp(e.StartTime/oldDuration,0,1)*clip.Clip.Duration;e.EndTime=Math.Clamp(e.EndTime/oldDuration,0,1)*clip.Clip.Duration;}
            int hits=0;
            foreach(var e in clip.Events.Where(e=>e.Type==1)) {
                var bytes=e.GetParameterBytes(false);Need(bytes.Length==12&&BitConverter.ToInt32(bytes)==0&&BitConverter.ToInt32(bytes,4)==0,"unsupported source player contact routing "+from+": "+Convert.ToHexString(bytes));
                var hit=new TAE.Event(e.StartTime,e.EndTime,1,0,bytes.Concat(new byte[4]).ToArray(),false);
                hit.Group=new TAE.EventGroup(1);keep.Add(hit);hits++;
            }
            row.Events.Clear();row.Events.AddRange(keep);row.EventGroups.Clear();row.EventGroups.AddRange(keep.Select(e=>e.Group).Where(g=>g!=null).Distinct()!);
            generated.Add(bank*1000000+action,NativeAnimation.Convert(clip.Clip,skeleton,bones,1,preserveSourceWorld:true));
            bindings.Add(new(bank,action,from,clip.MotionID,clip.Clip.Duration,hits));
        }
        foreach(var (action,from) in Attacks)if(owned[WeaponBank].Animations.Any(a=>a.ID==action))Source(WeaponBank,action,from);
        foreach(var (action,from) in Holds){Source(OneHandBank,action,20000+from);Source(BothHandsBank,action,120000+from);}
        // The split loader is declared by its TXT member; retain its retail
        // binder header and empty div/version records, with globally unique IDs.
        var split=carrier!;split.Files.RemoveAll(f=>f.ID>=1000000000);
        var required=appended.AnimationNames.Select(n=>int.Parse(n.AsSpan(1,3))*1000000+int.Parse(n.AsSpan(5,6))).Concat(generated.Keys).ToHashSet();int copies=0;
        foreach(int id in required.Order()) {
            int bank=id/1000000,action=id%1000000;byte[] bytes;
            if(!generated.TryGetValue(id,out bytes!)) {
                int from=bankMap.Single(p=>p.Value==bank).Key*1000000+action,physical=Physical(from);Need(motions.TryGetValue(physical,out var file),"native private fallback motion missing: "+from);bytes=file!.Bytes.ToArray();copies++;
            }
            split.Files.Add(new BinderFile(SoulsFormats.Binder.FileFlags.Flag1,1000000000+id,@"N:\SPRJ\data\INTERROOT_ps4\chr\c0000\hkx\"+$"a{bank:D3}\\a{bank:D3}_{action:D6}.hkx",bytes));
        }
        foreach(var (bank,tae) in owned)main.Files.Add(new BinderFile(SoulsFormats.Binder.FileFlags.Flag1,5000000+bank,@"N:\SPRJ\data\INTERROOT_ps4\chr\c0000\tae\"+$"a{bank:D2}.tae",tae.Write()));
        // PlayerIns18dfe42..18dfea0 loads a fixed14-path table at532fd60.
        // New TXT declarations do not extend that table. Append owned clips
        // to its existing hold/weapon binders, preserving all retail members.
        var nativeSplits=new Dictionary<string,byte[]>();
        foreach(string path in new[]{"chr/c0000_a8x.anibnd.dcx","chr/c0000_a9x.anibnd.dcx"}) {
            var bank=BND4.Read(native(path));bool hold=path.Contains("_a8x",StringComparison.Ordinal);
            foreach(var f in split.Files.Where(f=>f.ID>=1000000000&&((f.ID-1000000000)/1000000 is OneHandBank or BothHandsBank)==hold)) {
                Need(!bank.Files.Any(old=>old.ID==f.ID||Leaf(old.Name)==Leaf(f.Name)),"private clip already occupies native split");bank.Files.Add(new(f.Flags,f.ID,f.Name,f.Bytes.ToArray()));
            }
            bank.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));nativeSplits.Add(path,bank.Write(DCX.Type.DCX_DFLT_10000_44_9));
        }
        graph.Bytes=appended.Graph;character.Bytes=appended.Character;
        main.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));split.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));
        return new(main.Write(DCX.Type.DCX_DFLT_10000_44_9),split.Write(DCX.Type.DCX_DFLT_10000_44_9),nativeSplits,bindings,copies,appended.Selectors);
    }
}

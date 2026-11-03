// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class CatacombsSpecialCharacters {
    internal sealed record Result(CatacombsCreaturesCharacters.Result Character,NonHumanoidEnemies.Profile Profile,IReadOnlyList<int> Requests,int OpacityEvents,int RigidVertices,float HideDelay=0);
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs special character: "+why);}
    internal static NonHumanoidEnemies.Profile Profile(string model)=>model switch {
        "c2300"=>NonHumanoidEnemies.Demon,"c3300"=>NonHumanoidEnemies.Rat with{Source="c3300"},
        "c2794"=>new("c2794","c2630",263020,263020,"m24_01_00_01","m27_00_00_00",84,567),
        "c3501"=>new("c3501","c2610",261010,261010,"m22_00_00_00","m22_00_00_00",80,328),_=>throw new InvalidDataException("Unknown Catacombs special source")};
    static Func<string,byte[]?> RigidWisp(Func<string,byte[]?> source,out int vertices) {
        const string path="chr/c3501.chrbnd.dcx";var bank=BND3.Read(source(path)!);var entry=bank.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase));var model=FLVER2.Read(entry.Bytes);vertices=0;
        Need(model.Nodes.Count==3&&model.Meshes.Count==2,"source rigid skull population changed");
        foreach(var mesh in model.Meshes) {
            var normals=mesh.VertexBuffers.SelectMany(b=>model.BufferLayouts[b.LayoutIndex]).Where(l=>l.Semantic==FLVER.LayoutSemantic.Normal).ToArray();
            // The shipped Byte4B normal's W=127 is a normal component, not a
            // bone palette index. This rigid layout binds through NodeIndex1.
            Need(mesh.Dynamic==0&&!mesh.UseBoneWeights&&mesh.NodeIndex==1&&mesh.BoneIndices.SequenceEqual(new[]{1})&&normals.Length==1&&(int)normals[0].Type==19&&mesh.Vertices.All(v=>v.NormalW==127),"source rigid skull binding/layout changed");
            foreach(var v in mesh.Vertices){v.NormalW=0;vertices++;}
        }
        entry.Bytes=model.Write();var bytes=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);return p=>p==path?bytes:source(p);
    }
    internal static Result Build(Func<string,byte[]?> source,Func<string,byte[]> native,CatacombsSpecialSource.Family family,IReadOnlySet<int> masks,Dictionary<string,byte[]> files) {
        int rigid=0;var read=family.Source=="c3501"?RigidWisp(source,out rigid):source;var profile=Profile(family.Source);
        var active=family.Source switch {"c2300"=>Enumerable.Range(3000,10).Where(i=>i!=3006).ToHashSet(),"c2794"=>Enumerable.Range(3000,12).Where(i=>i!=3006).ToHashSet(),_=>new HashSet<int>{3000}};
        var extra=family.Source=="c3501"?new Dictionary<int,int>{{1500,8300}}:family.Source=="c2300"?new Dictionary<int,int>{{7000,9000}}:new Dictionary<int,int>();
        var actions=new CatacombsCreaturesActions(read,family.Source,0,active,extra);var built=CatacombsCreaturesCharacters.Build(read,native,family.Source,0,"c"+family.Model,masks,files,profile,actions);
        string path="chr/c"+family.Model+".anibnd.dcx";var bank=BND4.Read(files[path]);var entry=bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));var tae=TAE.Read(entry.Bytes);
        var nativeBank=BND4.Read(native("chr/c2730.anibnd.dcx"));var nativeTae=TAE.Read(nativeBank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);
        TAE.Event Carrier(int type,int group,int size){var e=nativeTae.Animations.SelectMany(a=>a.Events).First(e=>e.Type==type&&e.Group?.GroupType==group&&e.GetParameterBytes(false).Length==size);var copy=e.GetClone(false);copy.Group=e.Group!.GetClone();return copy;}
        var contactBank=BND4.Read(native("chr/"+profile.Native+".anibnd.dcx"));var contactTae=TAE.Read(contactBank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);var hitSource=contactTae.Animations.SelectMany(a=>a.Events).First(e=>e.Type==1&&e.Group?.GroupType==1&&e.GetParameterBytes(false).Length==16);var hit=hitSource.GetClone(false);hit.Group=hitSource.Group!.GetClone();var contacts=built.Contacts.ToHashSet();
        var opacity=Carrier(193,192,16);var request=Carrier(231,231,16);
        var fxBank=BND4.Read(native("chr/c3130.anibnd.dcx"));var fxTae=TAE.Read(fxBank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);
        TAE.Event FxCarrier(int type){var e=fxTae.Animations.SelectMany(a=>a.Events).First(e=>e.Type==type&&e.Group?.GroupType==96&&e.GetParameterBytes(false).Length==16);var copy=e.GetClone(false);copy.Group=e.Group!.GetClone();return copy;}
        var attached=FxCarrier(100);var charge=FxCarrier(96);int opacityCount=0;var requests=new HashSet<int>();
        float hideDelay=0;
        if(family.Source=="c3300") {
            var hide=SourceAnimation.Load(actions.Reader(read),family.Source,3000);var rig=bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
            foreach(var file in bank.Files.Where(f=>f.ID>=1000000000&&(f.ID-1000000000)%1000000==3000)) {var old=new NativeAnimation.Pack(file.Bytes.ToArray());float duration=old.Float(old.Object("hkaSplineCompressedAnimation")+16);file.Bytes=NativeAnimation.Convert(hide,rig,built.Skin.Bones,1,preserveSourceWorld:true);var a=tae.Animations.Single(a=>a.ID==file.ID-1000000000);foreach(var e in a.Events){e.StartTime=e.StartTime/duration*hide.Duration;e.EndTime=e.EndTime/duration*hide.Duration;}}
            var events=actions.Events(3000);hideDelay=events.Where(e=>e.Type==193).Max(e=>e.EndTime)-events.Single(e=>e.Type==231).StartTime;Need(Math.Abs(hideDelay-1.1f)<.00001f,"source lizard fade tail changed");
        }
        foreach(var a in tae.Animations) {
            var physical=bank.Files.SingleOrDefault(f=>f.ID==1000000000+a.ID);if(physical==null)continue;
            // Donor transient effects belong to its own equipment/species.
            // Only the audited source effect references below are installed.
            a.Events.RemoveAll(e=>e.Type is 193 or 231 or 66 or 67 or 96 or 100);int id=(int)(a.ID%1000000);var clip=SourceAnimation.Load(actions.Reader(read),family.Source,actions.SourceSlot(id));var p=new NativeAnimation.Pack(physical.Bytes.ToArray());float scale=p.Float(p.Object("hkaSplineCompressedAnimation")+16)/clip.Duration;
            if(id is <3000 or >=4000) {
                a.Events.RemoveAll(e=>e.Type is 1 or 2);foreach(var e in actions.Events(id).Where(e=>e.Type==1)){var data=e.GetParameterBytes(false);Need(data.Length==12,"source nonattack contact ABI");int judge=BitConverter.ToInt32(data,8);contacts.Add(new(judge,1));var payload=new byte[16];data.CopyTo(payload,0);var group=hit.Group!.GetClone();a.EventGroups.Add(group);a.Events.Add(new(e.StartTime*scale,e.EndTime*scale,1,hit.Unk04,payload,false){Group=group});}
            }
            if(family.Source=="c3300"&&id is 0 or 200 or 201) {var group=opacity.Group!.GetClone();a.EventGroups.Add(group);var payload=new byte[16];BitConverter.GetBytes(1f).CopyTo(payload,0);BitConverter.GetBytes(1f).CopyTo(payload,4);a.Events.Add(new(0,.1f,193,opacity.Unk04,payload,false){Group=group});}
            foreach(var fx in actions.Events(id).Where(e=>e.Type is 96 or 100)) {
                var data=fx.GetParameterBytes(false);Need(data.Length==12,"source attached FFX ABI");int effect=BitConverter.ToInt32(data);if(!CatacombsSpecialEffects.Attached.TryGetValue(effect,out int mapped))continue;
                int dummy=BitConverter.ToInt32(data,4);Need(dummy is >=0 and <1000,"source attached FFX socket");var payload=new byte[16];data.CopyTo(payload,0);BitConverter.GetBytes(mapped).CopyTo(payload,0);BitConverter.GetBytes(CatacombsCreaturesCharacters.ContactBase+dummy).CopyTo(payload,4);var prototype=fx.Type==100?attached:charge;var group=prototype.Group!.GetClone();a.EventGroups.Add(group);a.Events.Add(new(fx.StartTime*scale,fx.EndTime*scale,fx.Type,prototype.Unk04,payload,false){Group=group});
            }
            foreach(var e in actions.Events(id).Where(e=>e.Type is 193 or 231)) {
                var payload=new byte[16];var data=e.GetParameterBytes(false);Need(data.Length==(e.Type==193?8:4),"source opacity/request ABI");data.CopyTo(payload,0);var prototype=e.Type==193?opacity:request;
                if(e.Type==231){int value=BitConverter.ToInt32(data);Need((family.Source,value) is ("c3501",400) or ("c3300",1000),"unsupported source action request "+value);requests.Add(value);}else opacityCount++;
                var group=prototype.Group!.GetClone();a.EventGroups.Add(group);a.Events.Add(new(e.StartTime*scale,e.EndTime*scale,e.Type,prototype.Unk04,payload,false){Group=group});
            }
        }
        entry.Bytes=tae.Write();files[path]=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);return new(built with{Contacts=contacts.OrderBy(c=>c.Judge).ThenBy(c=>c.Kind).ToArray()},profile,requests.Order().ToArray(),opacityCount,rigid,hideDelay);
    }
}

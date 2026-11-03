// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
internal static class CatacombsCreaturesCharacters {
    internal const int ContactBase=1000;
    internal sealed record Contact(int Judge,int Kind);
    internal sealed record Result(SkinBake Skin,CatacombsCreaturesActions Actions,IReadOnlyList<Contact> Contacts,int Clips,int VisibleBones,CatacombsCreaturesPhysics.Report? Physics=null);
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs character: "+why);}
    static IReadOnlySet<int> ZeroTracks(NativeRigAppend.Plan plan)=>HollowSourceSkin.HiddenEquipmentBones(plan).Where(i=>System.Text.RegularExpressions.Regex.IsMatch(new string(plan.Added[i].Name[4..].Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray()),@"\A(?:rweapon\d*|lweapon\d*|lshield|sword|bpw00|bow|bowstring|arrow\d*|wcloth\d*)\z")).ToHashSet();
    static FLVER.Dummy Rebind(FLVER.Dummy dummy,FLVER2 source,NativeRigAppend.Plan plan,short reference) {
        var target=FLVER2.Read(plan.Model);var sw=SkinRetarget.World(source.Nodes);var tw=SkinRetarget.World(target.Nodes);
        var names=plan.Added.Select(b=>b.Name[4..]).ToHashSet(StringComparer.Ordinal);
        string root=plan.Added[0].Name[4..];int rootNode=source.Nodes.FindIndex(n=>n.Name==root);Need(rootNode>=0,"source animation root absent from mesh");
        int Owner(int node) {
            if(node<0)return rootNode;int count=0;
            while(node>=0) {Need(node<source.Nodes.Count&&++count<=source.Nodes.Count,"invalid dummy hierarchy");if(names.Contains(source.Nodes[node].Name))return node;
                node=source.Nodes[node].ParentIndex;}
            return rootNode;
        }
        int parent=Owner(dummy.ParentBoneIndex),index=target.Nodes.FindIndex(n=>n.Name=="DS1_"+source.Nodes[parent].Name);
        Need(index>=0&&Matrix4x4.Invert(sw[parent],out _),"dummy source target parent/singular");Matrix4x4.Invert(sw[parent],out var inverse);
        var before=dummy.ParentBoneIndex<0?Matrix4x4.Identity:sw[dummy.ParentBoneIndex];var delta=before*inverse;
        var result=new FLVER.Dummy(dummy){ReferenceID=reference,ParentBoneIndex=checked((short)index),Position=Vector3.Transform(dummy.Position,delta),Forward=Vector3.TransformNormal(dummy.Forward,delta),Upward=Vector3.TransformNormal(dummy.Upward,delta)};
        if(dummy.AttachBoneIndex>=0){int attach=Owner(dummy.AttachBoneIndex);int mapped=target.Nodes.FindIndex(n=>n.Name=="DS1_"+source.Nodes[attach].Name);Need(mapped>=0,"dummy target attachment missing");result.AttachBoneIndex=checked((short)mapped);}
        Need(Vector3.Distance(Vector3.Transform(dummy.Position,before),Vector3.Transform(result.Position,tw[index]))<.00001f&&
            Vector3.Distance(Vector3.TransformNormal(dummy.Forward,before),Vector3.TransformNormal(result.Forward,tw[index]))<.00001f&&
            Vector3.Distance(Vector3.TransformNormal(dummy.Upward,before),Vector3.TransformNormal(result.Upward,tw[index]))<.00001f,"dummy rest frame changed");return result;
    }
    internal static Result Build(Func<string,byte[]?> source,Func<string,byte[]> native,string sourceModel,int sourceOffset,string target,IReadOnlySet<int> masks,Dictionary<string,byte[]> files,NonHumanoidEnemies.Profile? nativeProfile=null,CatacombsCreaturesActions? prebuiltActions=null) {
        var actions=prebuiltActions??new CatacombsCreaturesActions(source,sourceModel,sourceOffset);var selected=actions.Reader(source);
        var profile=nativeProfile??new NonHumanoidEnemies.Profile(sourceModel,"c2610",261010,261010,"m22_00_00_00","m22_00_00_00",80,328);
        var authored=MapCharacters.Authored(source,sourceModel);var original=FLVER2.Read(authored.Model);
        var skin=NonHumanoidEnemies.Package(selected,native,profile,target,masks,files,actions.SourceSlot,adaptRig:(bake,plan)=> {
            Need(plan.Count<=512,"combined rig exceeds512 bones");
            foreach(var d in original.Dummies.Where(d=>d.ReferenceID>=0&&d.ReferenceID<1000).DistinctBy(d=>d.ReferenceID)) {
                short id=checked((short)(ContactBase+d.ReferenceID));Need(!bake.Model.Dummies.Any(x=>x.ReferenceID==id),"owned contact dummy occupied");
                bake.Model.Dummies.Add(Rebind(d,original,plan,id));
            }
        },sourceSample:HollowSourceSkin.Sample,zeroAddedBones:ZeroTracks);
        string path="chr/"+target+".anibnd.dcx";var bank=BND4.Read(files[path]);var entry=bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));var tae=TAE.Read(entry.Bytes);
        TAE.Event Prototype(TAE.Event e){var copy=e.GetClone(false);copy.Group=e.Group?.GetClone();return copy;}
        var hit=Prototype(tae.Animations.SelectMany(a=>a.Events).First(e=>e.Type==1&&e.Group?.GroupType==1));
        var ranged=BND4.Read(native("chr/c2630.anibnd.dcx"));var rangedTae=TAE.Read(ranged.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);var shot=Prototype(rangedTae.Animations.SelectMany(a=>a.Events).First(e=>e.Type==2&&e.Group?.GroupType==2));
        Need(hit.Group?.GroupType==1&&shot.Group?.GroupType==2&&hit.GetParameterBytes(false).Length==16&&shot.GetParameterBytes(false).Length==16,"native contact/event ABI changed "+hit.GetParameterBytes(false).Length+"/"+shot.GetParameterBytes(false).Length);var contacts=new HashSet<Contact>();
        foreach(var action in tae.Animations) {
            // c2610's torch particle is unrelated to all imported loadouts.
            action.Events.RemoveAll(e=>e.Type==100&&e.GetParameterBytes(false).Length>=4&&BitConverter.ToInt32(e.GetParameterBytes(false))==610250);
            int id=(int)(action.ID%1000000);if(id is <3000 or >=4000)continue;
            var sourceEvents=actions.Events(id);int clipSlot=actions.SourceSlot(id);var clip=SourceAnimation.Load(selected,sourceModel,clipSlot);
            action.Events.RemoveAll(e=>e.Type is 1 or 2||e.Type==0&&e.GetParameterBytes(false).Length>=4&&BitConverter.ToInt32(e.GetParameterBytes(false))==24);
            int physical=1000000000+checked((int)action.ID);var file=bank.Files.SingleOrDefault(f=>f.ID==physical);if(file==null)continue;
            var pack=new NativeAnimation.Pack(file.Bytes.ToArray());float duration=pack.Float(pack.Object("hkaSplineCompressedAnimation")+16),scale=duration/clip.Duration;
            foreach(var e in sourceEvents.Where(e=>e.Type is 1 or 2)) {
                var data=e.GetParameterBytes(false);Need(data.Length==(e.Type==1?12:16),"unverified source contact ABI");int judge=BitConverter.ToInt32(data,8);Need(judge is >=0 and <1000,"source judge budget exceeded");contacts.Add(new(judge,e.Type));
                var payload=new byte[16];data.CopyTo(payload,0);
                if(e.Type==2){int dummy=BitConverter.ToInt32(payload);Need(dummy is >=0 and <1000,"unverified source projectile socket");BitConverter.GetBytes(ContactBase+dummy).CopyTo(payload,0);}
                var prototype=e.Type==1?hit:shot;var group=prototype.Group?.GetClone();if(group!=null)action.EventGroups.Add(group);
                action.Events.Add(new(e.StartTime*scale,e.EndTime*scale,prototype.Type,prototype.Unk04,payload,false){Group=group});
            }
        }
        entry.Bytes=tae.Write();files[path]=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);
        if(sourceModel is "c2900" or "c2910") {
            MapSkeletonAnimations.Apply(selected,sourceModel,target,skin,files);
            bank=BND4.Read(files[path]);entry=bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));tae=TAE.Read(entry.Bytes);var rig=bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
            // Initial pile9001/awake9061 differs from lethal collapse9000 and
            // resurrection9060. Give the latter their existing native7006/7
            // playback slots with real source duration, rather than replaying
            // the initial awakening for every temporary defeat.
            foreach(var (slot,sourceID) in sourceModel=="c2900"?new[]{(7006,9000),(7007,9060)}:Array.Empty<(int,int)>()) {
                var clip=SourceAnimation.Load(selected,sourceModel,sourceID);var file=bank.Files.Single(f=>f.ID==1000000000+slot);
                var pack=new NativeAnimation.Pack(file.Bytes.ToArray());float old=pack.Float(pack.Object("hkaSplineCompressedAnimation")+16);
                file.Bytes=NativeAnimation.Convert(clip,rig,skin.Bones,1,preserveSourceWorld:true);var a=tae.Animations.Single(a=>a.ID==slot);
                a.Events.RemoveAll(e=>e.Type is 1 or 2 or 100);foreach(var e in a.Events){e.StartTime=Math.Clamp(e.StartTime/old,0,1)*clip.Duration;e.EndTime=Math.Clamp(e.EndTime/old,0,1)*clip.Duration;}
            }
            entry.Bytes=tae.Write();files[path]=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);
        }
        CatacombsCreaturesPhysics.Report? physics=null;
        if(profile.Native=="c2610"&&sourceModel is "c2900" or "c2910" or "c2650" or "c2930") {
            // The playback prefix controls locomotion; hurt bodies must follow
            // the appended source rig that actually drives the visible body.
            var nativeCharacter=BND4.Read(native("chr/c2610.chrbnd.dcx"));
            var completeRig=BND4.Read(files[path]).Files.Single(f=>f.ID==1000000).Bytes.ToArray();
            var converted=CatacombsCreaturesPhysics.Convert(nativeCharacter.Files.Single(f=>f.ID==300).Bytes.ToArray(),
                nativeCharacter.Files.Single(f=>Leaf(f.Name).EndsWith(".hkxpwv",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray(),
                CatacombsCreaturesPhysicsSource.Load(source,sourceModel),completeRig);
            string characterPath="chr/"+target+".chrbnd.dcx";var character=BND4.Read(files[characterPath]);
            character.Files.Single(f=>f.ID==300).Bytes=converted.Physics;
            character.Files.Single(f=>Leaf(f.Name).EndsWith(".hkxpwv",StringComparison.OrdinalIgnoreCase)).Bytes=converted.Pwv;
            files[characterPath]=character.Write(DCX.Type.DCX_DFLT_10000_44_9);physics=converted.Report;
        }
        return new(skin,actions,contacts.OrderBy(c=>c.Judge).ThenBy(c=>c.Kind).ToArray(),profile.Clips,SourceAnimation.Load(selected,sourceModel,0).Bones.Count,physics);
    }
}

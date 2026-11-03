// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// Existing Doll event-action nodes carry the exact source rescue/leave poses.
// No new graph selector or native player's global animation is overwritten.
internal static class MapDuskAnimations {
    internal sealed record Motion(int Native,int Source,int Physical,string Archive,int Frames,float Duration);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Dusk animations: "+why);}
    internal static IReadOnlyList<Motion> Apply(Func<string,byte[]?> source,Func<string,byte[]> native,SkinBake skin,Dictionary<string,byte[]> files) {
        string path="chr/c8910.anibnd.dcx";var bank=BND4.Read(files[path]);var skeleton=bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
        var taeFile=bank.Files.Single(f=>f.ID==3000000);var tae=TAE.Read(taeFile.Bytes);var result=new List<Motion>();var actions=new SourcePlayerActions(source);
        var opacityBank=BND4.Read(native("chr/c2730.anibnd.dcx"));var opacityTae=TAE.Read(opacityBank.Files.Single(f=>f.ID==3000000).Bytes);
        var opacity=opacityTae.Animations.Single(a=>a.ID==2200).Events.First(e=>e.Type==193&&e.GetParameterBytes(false).Length==16);
        Need(opacity.Group?.GroupType==192,"native opacity event group precedent changed");
        foreach(var (from,targets) in new[]{(7875,new[]{0,1000000,2000000}),(7876,new[]{7000}),(7877,new[]{7001}),(8289,new[]{7010})}) {
            var loaded=actions.Resolve(from);var clip=loaded.Clip;
            Need(clip.FrameCount<=256,"source rescue pose exceeds single-block encoder");
            var encoded=NativeAnimation.Convert(clip,skeleton,skin.Bones,skin.ProportionScale,preserveSourceWorld:true);
            foreach(int id in targets) {
                bank.Files.Single(f=>f.ID==1000000000+id).Bytes=encoded;
                // Omit the donor's effects, sound, AI signals and dialogue cues.
                var row=tae.Animations.Single(a=>a.ID==id);row.Events.Clear();row.EventGroups.Clear();
                foreach(var e in loaded.Events.Where(e=>e.Type is 0 or 193)) {
                    var data=e.GetParameterBytes(false);Need(data.Length==(e.Type==0?12:8),"source movement/opacity payload changed");
                    var group=e.Type==193?opacity.Group!.GetClone():new TAE.EventGroup(e.Type);row.EventGroups.Add(group);
                    row.Events.Add(new TAE.Event(Math.Min(e.StartTime,clip.Duration),Math.Min(e.EndTime,clip.Duration),e.Type,0,
                        [..data,..new byte[16-data.Length]],false){Group=group});
                }
                result.Add(new(id,from,loaded.MotionID,loaded.Archive,clip.FrameCount,clip.Duration));
            }
        }
        taeFile.Bytes=tae.Write();files[path]=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);return result;
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;using System.Security.Cryptography;using System.Text.Json;using System.Numerics;using Spline=StudioCore.Utilities.SplineCompressedAnimation;
var hashes=new Dictionary<string,string>();byte[] Read(string root,string rel){string path=Path.Combine(root,rel);var b=File.ReadAllBytes(path);hashes.TryAdd(path,Convert.ToHexStringLower(SHA256.HashData(b)));return b;}
byte[]? Source(string p)=>File.Exists(Path.Combine(args[0],p))?Read(args[0],p):null;byte[] Native(string p)=>Read(args[1],p);
var result=HydraCharacters.Bake(Source,Native,Native,args[3]);
void Check(bool b,string why){if(!b)throw new Exception(why);}
string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
PARAMDEF Def(string name)=>PARAMDEF.XmlDeserialize(Path.Combine(args[3],name+".xml"));
var chr=BND4.Read(result.Files["chr/c8830.chrbnd.dcx"]);var anim=BND4.Read(result.Files["chr/c8830.anibnd.dcx"]);
var oldAnim=BND4.Read(Native("chr/c2730.anibnd.dcx"));var originalRig=NativeAnimation.Skeleton(oldAnim.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
var full=NativeAnimation.Skeleton(anim.Files.Single(f=>f.ID==1000000).Bytes.ToArray());Check(full.Names.Length==356&&full.Names.Take(110).SequenceEqual(originalRig.Names),"Native bone prefix/annotation spelling changed");
foreach(string name in originalRig.Names.Where(n=>n.Any(char.IsControl)))Check(full.Names.Contains(name),"Literal native control character in name was stripped");
var visual=FLVER2.Read(chr.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);var srcModel=FLVER2.Read(BND3.Read(Source("chr/c3530.chrbnd.dcx")!).Files.Single(f=>Leaf(f.Name)=="c3530.flver").Bytes);
Check(visual.Meshes.Count==26&&visual.Meshes.All(m=>SkinRetarget.MaximumBoneIndex(visual,m)==ushort.MaxValue),"Full geometry/UShort4 layout lost");
for(int i=0;i<26;i++)Check(visual.Materials[visual.Meshes[i].MaterialIndex].Name==srcModel.Materials[srcModel.Meshes[i].MaterialIndex].Name,"Severable head material mask name lost");
var sourceDummies=srcModel.Dummies.GroupBy(d=>d.ReferenceID).ToDictionary(g=>g.Key,g=>g.Count());
foreach(var (id,count)in sourceDummies)Check(visual.Dummies.Count(d=>d.ReferenceID==HydraCharacters.Socket(id))==count,"Source socket multiplicity changed");
int high=visual.Meshes.Sum(m=>m.Vertices.Sum(v=>Enumerable.Range(0,4).Count(i=>v.BoneWeights[i]>0&&v.BoneIndices[i]>255)));Check(high>0,"Large render indices were truncated");
var tae=TAE.Read(anim.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);Check(tae.ID==208830&&tae.Animations.Count==226&&anim.Files.Count(f=>f.ID>=1000000000)==226,"Owned action/alias population changed");
var actions=new SourceCharacterActions(Source,"c3530");var ownedJudges=result.ParameterReport.Judges.ToHashSet();int hits=0,shots=0;
foreach(var row in tae.Animations){Check(row.MiniHeader is TAE.Animation.AnimMiniHeader.Standard h&&!h.ImportsHKX&&h.ImportHKXSourceAnimID==-1,"Action escapes owned motion bank");
 foreach(var e in row.Events){Check(e.Group!=null&&row.EventGroups.Contains(e.Group),"Event has no owned native group");var bytes=e.GetParameterBytes(false);
  Check(e.Type is 0 or 1 or 2,"Donor/source unsupported event residue");
  if(e.Type==1){hits++;Check(bytes.Length>=16&&bytes.Length<=32&&bytes.AsSpan(16).IndexOfAnyExcept((byte)0)<0&&row.ID%1000000 is >=3000 and <=3008&&ownedJudges.Contains(BitConverter.ToInt32(bytes,8)),$"Unowned melee judge action{row.ID} len{bytes.Length} judge{BitConverter.ToInt32(bytes,8)}");}
  if(e.Type==2){shots++;Check(bytes.Length>=16&&bytes.Length<=32&&bytes.AsSpan(16).IndexOfAnyExcept((byte)0)<0&&row.ID%1000000 is >=3000 and <=3008&&ownedJudges.Contains(BitConverter.ToInt32(bytes,8))&&sourceDummies.ContainsKey((short)(BitConverter.ToInt32(bytes)-500)),"Unowned projectile judge/socket");}
 }
}
Matrix4x4 Sample(Spline.TransformTrack track,float frame){var p=track.StaticPosition;var scale=track.StaticScale;if(track.HasSplinePosition)p=new(track.SplinePosition.GetValueX(frame)??p.X,track.SplinePosition.GetValueY(frame)??p.Y,track.SplinePosition.GetValueZ(frame)??p.Z);if(track.HasSplineScale)scale=new(track.SplineScale.GetValueX(frame)??scale.X,track.SplineScale.GetValueY(frame)??scale.Y,track.SplineScale.GetValueZ(frame)??scale.Z);return Matrix4x4.CreateScale(scale)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(track.HasSplineRotation?track.SplineRotation.GetValue(frame):track.StaticRotation))*Matrix4x4.CreateTranslation(p);}
float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(p=>Vector3.Distance(Vector3.Transform(p,a),Vector3.Transform(p,b)));
float maxPose=0;int stationary=0;
foreach(var file in anim.Files.Where(f=>f.ID>=1000000000)) {
 var bytes=file.Bytes.ToArray();var p=new NativeAnimation.Pack(bytes);int a=p.Object("hkaSplineCompressedAnimation"),id=file.ID-1000000000;Check(p.Int(a+20)==356,"Physical clip lost source tracks");
 try{int frame=p.Object("hkaDefaultAnimatedReferenceFrame");var samples=p.Array(frame+56,16,65536);Check(bytes.AsSpan(p.Data+samples.At,samples.Count*16).IndexOfAnyExcept((byte)0)<0,"Extracted motion is not stationary");}catch(InvalidDataException){ }
 stationary++;
 if(id is not(3000 or 3008 or 7005))continue;
 int sourceId=id==7005?8000:id;var from=actions.Resolve(sourceId);Check(Math.Abs(p.Float(a+16)-from.Clip.Duration)<.00001f,"Source duration not used");
 var payload=p.Array(a+152,1,64*1024*1024);int frames=p.Int(a+56);var decoded=Spline.ReadSplineCompressedAnimByteBlock(false,bytes.AsSpan(p.Data+payload.At,payload.Count).ToArray(),356,p.Int(a+60));
 foreach(int frame in new[]{0,(frames-1)/2,frames-1}){int block=frame/255,local=frame-block*255;var expected=from.Clip.Sample(frame/(float)(frames-1)*from.Clip.Duration);Check(Matrix4x4.Invert(Sample(decoded[block][0],local),out var inverse),"Singular native root sample");expected[0]*=inverse;
  for(int b=0;b<246;b++){float error=Error(expected[b],Sample(decoded[block][110+b],local));maxPose=Math.Max(maxPose,error);Check(error<.006f,"Decoded source attack/recoil pose changed");}
 }
 var oldFile=oldAnim.Files.Single(f=>f.ID==file.ID);var oldBytes=oldFile.Bytes.ToArray();var op=new NativeAnimation.Pack(oldBytes);int oa=op.Object("hkaSplineCompressedAnimation");var oldAnnotations=op.Array(oa+40,24,512);var annotations=p.Array(a+40,24,512);Check(oldAnnotations.Count==annotations.Count,"Original annotation population changed");
 for(int b=0;b<annotations.Count;b++)Check(op.String(oldAnnotations.At+b*24)==p.String(annotations.At+b*24),"Original annotation literal name changed");
}
var game=BND4.Read(result.Files["param/gameparam/gameparam.parambnd.dcx"]);var nativeGame=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx"));
PARAM Raw(BinderFile f){var h=PARAM.Read(f.Bytes);return ImportParam.Read(f.Bytes,h.ParamType,(int)h.DetectedSize,h.ParamdefDataVersion);}
foreach(string table in new[]{"NpcParam","NpcThinkParam","RagdollParam","SpEffectParam","AtkParam_Npc","BehaviorParam","Bullet"}){var old=Raw(nativeGame.Files.Single(f=>Leaf(f.Name)==table+".param"));var next=Raw(game.Files.Single(f=>Leaf(f.Name)==table+".param"));var priorIds=old.Rows.Select(r=>r.ID).ToHashSet();var retained=next.Rows.Where(r=>priorIds.Contains(r.ID)).ToArray();Check(retained.Length==old.Rows.Count,"Original row multiplicity changed");for(int i=0;i<retained.Length;i++){var r=old.Rows[i];var n=retained[i];Check(r.ID==n.ID&&r.Name==n.Name&&ImportParam.Payload(r).SequenceEqual(ImportParam.Payload(n)),"Original opaque row changed: "+table+"/"+r.ID);}}
PARAM Typed(string name,string? def=null){var p=PARAM.Read(game.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(Def(def??name));return p;}
var npcs=Typed("NpcParam");var body=npcs.Rows.Single(r=>r.ID==883000);var head=npcs.Rows.Single(r=>r.ID==883001);Check(body.Name=="Imported Hydra c3530"&&Convert.ToInt32(body["hp"].Value)==1680&&Convert.ToInt32(body["teamType"].Value)==23,"Owned body stats/context changed");
Check(Convert.ToInt32(head["hp"].Value)==1&&Convert.ToInt32(head["getSoul"].Value)==0&&Enumerable.Range(0,8).All(i=>Convert.ToInt32(head["spEffectID"+i].Value)==-1),"Detached helper acquired body scaling/drops");
var brains=Typed("NpcThinkParam");Check(Convert.ToInt32(brains.Rows.Single(r=>r.ID==883000)["logicId"].Value)==10000&&Convert.ToInt32(brains.Rows.Single(r=>r.ID==883000)["battleGoalID"].Value)==883000&&Convert.ToInt32(brains.Rows.Single(r=>r.ID==883001)["logicId"].Value)==11000,"Owned AI routing changed");
var effects=Typed("SpEffectParam","SpEffect");for(int slot=0;slot<7;slot++){var e=effects.Rows.Single(r=>r.ID==883200+slot);Check(Convert.ToInt32(e["addBehaviorJudgeId_condition"].Value)==slot+1&&Convert.ToInt32(e["addBehaviorJudgeId_add"].Value)==0&&Convert.ToSingle(e["effectEndurance"].Value)==-1,"Head behavior suppression changed");}
Check(Convert.ToSingle(effects.Rows.Single(r=>r.ID==883207)["maxHpRate"].Value)==1.5f,"Source HP scaling changed");
foreach(int id in HydraParameters.EffectIds.Values)Check(Convert.ToInt32(effects.Rows.Single(r=>r.ID==id)["effectTargetSelfTarget"].Value)==1,"Native resident self-target gate omitted");
var bullets=Typed("Bullet","BulletParam");foreach(int id in result.Ids.Bullets.Values){var r=bullets.Rows.Single(r=>r.ID==id);int attack=Convert.ToInt32(r["atkId_Bullet"].Value),child=Convert.ToInt32(r["HitBulletID"].Value);Check((attack<=0||result.Ids.Attacks.Values.Contains(attack))&&(child<0||result.Ids.Bullets.Values.Contains(child)),"Projectile escapes owned closure");foreach(string f in new[]{"sfxId_Bullet","sfxId_Hit","sfxId_Flick"})Check(Convert.ToInt32(r[f].Value)<=0||result.RequiredNativeSfx.Contains(Convert.ToInt32(r[f].Value)),"Projectile has unresolved source effect ID");}
var checks=new{stationary,hits,shots,sourceDummies=srcModel.Dummies.Count,sourceDummyIDs=sourceDummies.Count,highWeightedIndices=high,maxPose,originalRowsPreserved=true};
Directory.CreateDirectory(args[2]);foreach(var (path,bytes)in result.Files){string output=Path.Combine(args[2],path);Directory.CreateDirectory(Path.GetDirectoryName(output)!);File.WriteAllBytes(output,bytes);}
File.WriteAllText(Path.Combine(args[2],"report.json"),JsonSerializer.Serialize(new{checks,result.PhysicsReport,result.PwvReport,result.AnimationReport,result.ParameterReport,result.RequiredNativeSfx,result.Limitations,inputs=hashes},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
foreach(var (path,hash)in hashes)if(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))!=hash)throw new Exception("Original input changed: "+path);
Console.WriteLine($"PASS Hydra assembled owned package; {result.AnimationReport.OwnedActions} actions, {result.Ids.Attacks.Count} attacks, {result.Ids.Bullets.Count} bullets; {hashes.Count} original inputs unchanged. Native gameplay untested.");

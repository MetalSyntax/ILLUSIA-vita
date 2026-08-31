// CMvMap::PreLoad()

void CMvMap::PreLoad(void)

{
  CMvMap *in_r0;
  GVXLLoader *pGVar1;
  uint uVar2;
  void *__src;
  int iVar3;
  undefined4 *puVar4;
  void *__dest;
  uint uVar5;
  int iVar6;
  int iVar7;
  uint uVar8;
  EnumObjectLayer EVar9;
  undefined4 *this;
  
  uVar8 = (uint)(byte)in_r0[4];
  pGVar1 = (GVXLLoader *)CMvXlsMgr::GetTbl(CGsSingleton<CMvXlsMgr>::ms_pSingleton,6);
  GVXLLoader::GetVal(pGVar1,1,uVar8);
  pGVar1 = (GVXLLoader *)CMvXlsMgr::GetTbl(CGsSingleton<CMvXlsMgr>::ms_pSingleton,6);
  uVar2 = GVXLLoader::GetVal(pGVar1,3,uVar8);
  if (uVar2 - 1 < 2) {
    uVar5 = Random(100);
    uVar2 = uVar2 & -((uint)(uVar5 < 0x46) - ((int)uVar5 >> 0x1f));
  }
  SetWeaterType(in_r0,uVar2);
  __src = (void *)LoadMapData(in_r0,uVar8);
  memcpy(in_r0 + 200,__src,0x14);
  iVar7 = *(int *)(in_r0 + 0xd4);
  iVar6 = *(int *)(in_r0 + 0xd0);
  iVar3 = __aeabi_idiv(iVar7 * iVar6,100);
  iVar3 = iVar3 + 3;
  if (0x3f < iVar3) {
    iVar3 = 0x3f;
  }
  *(int *)(in_r0 + 0x310) = iVar3;
  LoadTileImage(in_r0,*(int *)(in_r0 + 0xd8));
  if (*(int *)(in_r0 + 800) != 0) {
    MC_knlFree();
    *(undefined4 *)(in_r0 + 800) = 0;
  }
  memcpy(in_r0 + 0x31c,(void *)((int)__src + 0x14),4);
  uVar2 = 0x18;
  if (*(int *)(in_r0 + 0x31c) != 0) {
    __dest = (void *)MC_knlCalloc(*(int *)(in_r0 + 0x31c) * 10);
    *(void **)(in_r0 + 800) = __dest;
    memcpy(__dest,(void *)((int)__src + 0x18),*(int *)(in_r0 + 0x31c) * 10);
    uVar2 = *(int *)(in_r0 + 0x31c) * 10 + 0x18;
  }
  iVar3 = *(int *)(in_r0 + 0xcc);
  puVar4 = (undefined4 *)operator_new__((iVar3 * 3 + 1) * 8);
  *puVar4 = 0x18;
  puVar4[1] = iVar3;
  puVar4 = puVar4 + 2;
  this = puVar4;
  for (; iVar3 != 0; iVar3 = iVar3 + -1) {
    CMvLayer::CMvLayer((CMvLayer *)this);
    this = this + 6;
  }
  *(undefined4 **)(in_r0 + 0xe4) = puVar4;
  if (0 < *(int *)(in_r0 + 0xcc)) {
    iVar3 = 0;
    EVar9 = 0;
    while( true ) {
      uVar2 = CMvLayerData::PreLoad
                        ((CMvLayerData *)((int)puVar4 + iVar3),EVar9,iVar7 * iVar6,(long)__src,uVar2
                        );
      EVar9 = EVar9 + 1;
      iVar3 = iVar3 + 0x18;
      if (*(int *)(in_r0 + 0xcc) <= (int)EVar9) break;
      puVar4 = *(undefined4 **)(in_r0 + 0xe4);
    }
  }
  GcxFreeResData(__src);
  return;
}

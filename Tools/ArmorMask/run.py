"""Tuning previews: composites ~20 real plate/mail sets (and helms/shoulders) from the client MPQs and writes
colour | mask side by side to $ARMORMASK_OUT (default ./out). Cyan = plate metal, orange = mail metal, grey = not metal."""
import sys, os
import numpy as np
from PIL import Image
import mpq, compose
legs = {'wrath_t2': 33986, 'judgement_t2_red': 33637, 'lawbringer_gold': 31352, 'destroyer_t5': 42298, 'crystalforge_t5_teal': 42309, 'dreadnaught_t3': 35051, 'redemption_t7': 55179, 'dreadnaught_t7': 55376, 'justicar_t4': 40478, 'cryptstalker_t3': 35413, 'giantstalker_t1': 32029, 'beaststalker_green': 31403, 'earthshatter_t3': 35754, 'wrathful_chain': 65465}
SETS = [('wrath_t2', [33983]), ('judgement_t2_red', [33635]), ('lawbringer_gold', [31505]), ('lightbringer_t6', [46350]),
        ('destroyer_t5', [42297]), ('crystalforge_t5_teal', [42306]), ('dreadnaught_t3', [35049]), ('redemption_t7', [55173]),
        ('dreadnaught_t7', [55369]), ('justicar_t4', [40474]), ('scarlet_crusade', [3519, 15816, 19049, 15817]),
        ('scarlet_commander', [56373]), ('chainmail', [1019]), ('cryptstalker_t3', [35415]), ('giantstalker_t1', [32022]),
        ('beaststalker_green', [31402]), ('earthshatter_t3', [35752]), ('wrathful_chain', [65463])]
ex, _ = mpq.dbc('CreatureDisplayInfoExtra.dbc')
e = ex[346]  # Stormwind City Guard: helm, shoulder, shirt, chest, belt, legs, boots, wrist, gloves, tabard, cape
it = e[8:19]
order = [2, 5, 6, 3, 7, 8, 4, 9]
SETS.append(('stormwind_guard_tabard', [it[i] for i in order if it[i]]))
skin = compose.blp('Character\\Human\\Male\\HumanMaleSkin00_00.blp')
SETS = [(n, ([legs[n]] if n in legs else []) + d) for n, d in SETS]
only = sys.argv[1:]
stats = []
for name, ds in SETS:
    if only and name not in only: continue
    col, mask = compose.build(ds, skin)
    compose.preview(col, mask, os.path.join(compose.OUT, name + '.png'))
    np.save(os.path.join(compose.OUT, name + '_mask.npy'), mask)
    print(name, ds, 'metal %.2f' % np.maximum(mask[..., 0], mask[..., 1]).mean())

ATT = [('helm_wrath', 34215, 'Head'), ('shoulder_wrath', 34253, 'Shoulder'), ('helm_judgement', 45888, 'Head'), ('shoulder_judgement', 34258, 'Shoulder'),
       ('helm_lawbringer', 31506, 'Head'), ('shoulder_lawbringer', 31510, 'Shoulder'), ('helm_crystalforge', 44050, 'Head'),
       ('shoulder_crystalforge', 42310, 'Shoulder'), ('helm_lightbringer', 45684, 'Head'), ('shoulder_lightbringer', 45884, 'Shoulder'),
       ('helm_cryptstalker', 35601, 'Head'), ('helm_giantstalker', 32028, 'Head'), ('helm_redemption_t7', 57547, 'Head')]
for name, d, fold in ATT:
    if only and name not in only: continue
    t = compose.S(compose.idi[d][3])
    r = compose.attach('Item\\ObjectComponents\\%s\\%s.blp' % (fold, t), compose.kind(d))
    if r is None: continue
    compose.preview(r[0], r[1], os.path.join(compose.OUT, name + '.png'), 192)
    print(name, 'metal %.2f' % max(r[1][..., 0].mean(), r[1][..., 1].mean()))

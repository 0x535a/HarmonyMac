#!/usr/bin/env python3
"""Verify RenderService's two real scanouts; command markers alone are insufficient."""
import argparse
import json
from pathlib import Path
from PIL import Image

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('evidence',type=Path)
a=p.parse_args()
report={'passed':True,'frames':[]}
for index,expected in enumerate([(204,34,68),(34,85,204)]):
    path=a.evidence/f'scanout-{index}.ppm'
    if not path.is_file():
        report['frames'].append({'path':str(path),'missing':True});report['passed']=False;continue
    image=Image.open(path).convert('RGB')
    matched=sum(all(abs(pixel[c]-expected[c])<=1 for c in range(3)) for pixel in image.getdata())
    total=image.width*image.height
    ok=image.size==(1280,800) and matched==total
    report['frames'].append({'path':str(path),'size':list(image.size),'expected_rgb':expected,
                             'matched':matched,'pixels':total,'passed':ok})
    report['passed'] &= ok
(a.evidence/'scanout-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
raise SystemExit(0 if report['passed'] else 1)

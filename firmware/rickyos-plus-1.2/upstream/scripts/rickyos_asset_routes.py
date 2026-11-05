"""Download routes for the RickyOS asset catalogs (fonts, standby pictures).

Every catalog is published to the same files on up to four hosts. Each route gets its
own manifest whose baseUrl stays on that host, and whose "mirrors" list the others,
which the reader tries when a file fails:

  github    the GitHub release (authoritative, slow or blocked from mainland China)
  jsdelivr  cdn.jsdelivr.net over the GitHub repository
  fastly    fastly.jsdelivr.net, the same files through another CDN
  cos       the RickyOS Tencent COS bucket in Guangzhou (fast in mainland China),
            only when a COS base is given; layout <cos>/<kind>/<tag>/<file>

The firmware reads cos.json from <cos>/<kind>/cos.json (see src/network/RickyCos.h).
"""
import re

COS_BASE_PATTERN = re.compile(r"^https://[a-z0-9-]+\.cos\.[a-z0-9-]+\.myqcloud\.com$")


def check_cos_base(cos_base):
    """The bucket's default HTTPS domain, no trailing slash, or None."""
    if cos_base is None:
        return None
    if not COS_BASE_PATTERN.match(cos_base):
        raise ValueError(f"COS base must look like https://<bucket>-<appid>.cos.<region>.myqcloud.com: {cos_base}")
    return cos_base


def routes(kind, repo, tag, cos_base=None, repo_dir=None):
    """[(manifest name, baseUrl, mirrors)] for one release of a catalog.

    kind: "fonts" or "wallpapers" (the folder the files sit in, in the repo and on COS).
    repo_dir: folder of the files inside the GitHub repository, for jsDelivr (default kind).
    """
    cos_base = check_cos_base(cos_base)
    folder = repo_dir or kind
    github = f"https://github.com/{repo}/releases/download/{tag}/"
    jsdelivr = f"https://cdn.jsdelivr.net/gh/{repo}@{tag}/{folder}/"
    fastly = f"https://fastly.jsdelivr.net/gh/{repo}@{tag}/{folder}/"
    cos = f"{cos_base}/{kind}/{tag}/" if cos_base else None
    with_cos = [cos] if cos else []
    result = [
        (f"{kind}.json", github, [jsdelivr, fastly] + with_cos),
        ("mirror.json", jsdelivr, with_cos + [fastly, github]),
    ]
    if cos:
        result.append(("cos.json", cos, [jsdelivr, fastly, github]))
    return result

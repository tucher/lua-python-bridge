def fuuu():
    return "FUUUU"


def call_funct(f, *args):
    return f(*args)

from _struct import *

def test_requests():
    import requests
    return requests.get("http://ifconfig.me").text

def test_threading():
    import threading
    import time
    import logging
    def thread_function(name):
        logging.info("Thread %s: starting", name)
        time.sleep(2)
        logging.info("Thread %s: finishing", name)
    format = "%(asctime)s: %(message)s"
    logging.basicConfig(format=format, level=logging.INFO,
                        datefmt="%H:%M:%S")

    logging.info("Main    : before creating thread")
    x = threading.Thread(target=thread_function, args=(1,))
    logging.info("Main    : before running thread")
    x.start()
    logging.info("Main    : wait for the thread to finish")
    x.join()
    logging.info("Main    : all done")

import asyncio
loop = asyncio.get_event_loop()

def test_async_io():
    import aiohttp
    
    async def download_test(url):
        async with aiohttp.ClientSession() as session:
            async with session.get(url) as response:
                print("Status:", response.status)
                print("Content-type:", response.headers['content-type'])
                html = await response.text()
                print("Body:", html[:15], "...")

                print(f"'{url}' done")
    async def main():
        await asyncio.gather(
            download_test('http://python.org'),
            download_test('http://ifconfig.me'),
            download_test('http://tuchkov.org')
        )
    loop.run_until_complete(main())

def test_server():
    from aiohttp import web
    from asyncio import sleep, gather
    async def handle(request):
        name = request.match_info.get('name', "Anonymous")
        text = "Hello, " + name
        return web.Response(text=text)
    async def runserver(port):
        app = web.Application()
        app.add_routes([web.get('/', handle),
                        web.get('/{name}', handle)])
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, 'localhost', port)
        await site.start()
        await sleep(10)
        await runner.cleanup()

    async def main():
        await gather(
            runserver(8080),
            runserver(8081),
            runserver(8082)
        )
    loop.run_until_complete(main())

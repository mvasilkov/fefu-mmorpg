define(["phaser", "utils", "ws"], function ($,utils,ws) {
    var game;    
    function Start() { 
        game = new Phaser.Game(1024,600, Phaser.AUTO, 'phaser-example', { preload: preload, create: create, update: update});
    }
    function requestDictionary() {
    return JSON.stringify( {
        ".": "grass",
         "#": "wall",
        })
    }
                    
    function requestLook() {
        return JSON.stringify({"map":
            [
                ["#", "#", ".", "#", "#", "#"],
                [".", ".", ".", ".", ".", "."],
                [".", ".", ".", ".", ".", "."],
                [".", ".", ".", ".", ".", "."]
            ],
            "actors": [
                        {"type": "player",
                        "id": 1,
                        "x": 4,
                        "y": 2 }           
                       ]        
            })
    }
                    
    var dictionary;
                    
    function DictionaryParser() {
        var data = JSON.parse(requestDictionary());
        if (data["."])
             game.load.image(data["."], 'assets/'+data["."]+'.png');
        if (data["#"])    
            game.load.image(data["#"], 'assets/'+data["#"]+'.png');
        dictionary = data;   
    }
                    
    function preload() {
        DictionaryParser();
        game.load.image('player', 'assets/tank4.png');
    }
                    
    function createPlayer(x, y) {
        var bet = game.add.sprite(x, y, 'player');
        bet.anchor.setTo(0.5, 0.5);
        bet.body.collideWorldBounds = true;
        bet.body.bounce.setTo(1, 1);
        bet.body.immovable = true
        return bet;
    }
                    
    var player;
    var upKey;
    var downKey;
    var leftKey;
    var rightKey;
    var stepX = 171;
    var stepY = 160;
    var object = new Array();
                    
    function Rendering() {
        var data = JSON.parse(requestLook());
        var map = data.map;
        var ans = new Array();
        for (var i = 0; i < map.length; i++)  {
            for (var j = 0; j < map[i].length; j++ ) {
                if (map[i][j] == "#"){
                    ans.push(game.add.sprite(j*stepY, i*stepX, 'wall'));
                }
            }
        }
        map = data.actors;
        for (var i = 0; i < map.length; i++)  { 
            var play = map[i];
            ans.push(game.add.sprite(play.x*stepY, play.y*stepX, play.type));
        }
        return ans;
    }
                    
    function create() {
        game.add.tileSprite(0, 0, 1024, 640, 'grass');
        object = Rendering();
        player = createPlayer(game.world.centerX, game.world.centerY);
        upKey = game.input.keyboard.addKey(Phaser.Keyboard.UP);
        downKey = game.input.keyboard.addKey(Phaser.Keyboard.DOWN);
        leftKey = game.input.keyboard.addKey(Phaser.Keyboard.LEFT);
        rightKey = game.input.keyboard.addKey(Phaser.Keyboard.RIGHT);
    }


    function update() {
        object = Rendering();
        if (upKey.isDown)
        {
            //if (Move(north).result == "ok") {
                player.y--;
            //} else  {
                            
             // }
                            
        }
        else if (downKey.isDown)
        {
            // if (Move(south).result == "ok") {
                player.y++;
            // } else  {
                                
                // }    
        }

        if (leftKey.isDown)
        {
            // if (Move(west).result == "ok") {
                player.x--;
            // } else  {
                            
            //  }
        }
        else if (rightKey.isDown)
        {
            // if (Move(east).result == "ok") {
                player.x++;
            //  } else  {
                            
            //  }
        }
                        

    } 

});